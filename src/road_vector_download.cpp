#ifdef ARDUINO
#include "road_vector_download.h"
#include "road_vector.h"
#include "road_vector_gzip.h"
#include "road_cache_clear.h"
#include "road_http_body.h"
#include "road_progress.h"
#include "roads_sd.h"
#include "theme_sd.h"
#include "sdcard.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/idf_additions.h>
#include <miniz.h>
#include <atomic>
#include <memory>

namespace road_vector_download {
namespace {
using namespace road_vector;
std::atomic<uint32_t> generation{1};
std::atomic<uint32_t> liveBytes{0};
std::atomic<uint32_t> liveCapacity{0};
std::atomic<int> liveLength{-1};
std::atomic<road_progress::Phase> livePhase{road_progress::Phase::waiting};
uint32_t workerStarted=0,workerGeneration=0;
struct Request { Tile tile;uint32_t source,gen;char url[896]; };
struct Result {
    uint8_t *data=nullptr;size_t len=0,bytes=0,inputCapacity=0;uint32_t gen=0,networkMs=0,decodeMs=0;
    int http=0,contentLength=-1;unsigned readCalls=0,idlePolls=0;
    bool chunked=false;const char *encoding="identity";
    Stats stats; const char *error="";
};
QueueHandle_t results=nullptr,requests=nullptr;
TaskHandle_t workerTask=nullptr;
constexpr uint32_t kWorkerStackBytes=24*1024;
unsigned workerCreates=0,workerRequests=0;
std::atomic<bool> workerExternalStack{false};
bool worker=false,active=false,paused=false,inspecting=false,saving=false,cleanup=false;
bool fetchEnabled=true;
String endpoint=ROAD_VECTOR_URL;
uint32_t source=0,requestAt=0,checkCrc=~0u,expectedCrc=0;
uint32_t transferred=0;
Tile tiles[MAX_TILES];size_t count=0,index=0,doneCount=0,offset=0;
bool done[MAX_TILES]={},checked[MAX_TILES]={};uint8_t attempts[MAX_TILES]={};uint32_t retryAt[MAX_TILES]={};
double lastLat=999,lastLon=999,lastRange=-1;
File file;Result pending,last;
char finalPath[96]={},tempPath[104]={};
const char *state="idle",*error="";
uint32_t saveStart=0,saveMs=0;
road_cache::ClearJob clearJob;
struct CacheStorage {
    File dir;
    road_cache::Scan open(const char *path) {
        close();
        if(!SD.exists(path))return road_cache::Scan::missing;
        dir=SD.open(path,FILE_READ);
        if(!dir||!dir.isDirectory()){close();return road_cache::Scan::error;}
        return road_cache::Scan::entry;
    }
    road_cache::Scan next(char *name,size_t cap,bool &directory) {
        File entry=dir.openNextFile();if(!entry)return road_cache::Scan::end;
        const char *n=entry.name();directory=entry.isDirectory();
        if(!n||strlen(n)>=cap){entry.close();return road_cache::Scan::error;}
        strcpy(name,n);entry.close();return road_cache::Scan::entry;
    }
    bool remove(const char *path){return SD.remove(path);}
    void close(){if(dir)dir.close();}
} clearStorage;

bool poll(void *ctx) {
    delay(1);return generation.load()==*static_cast<uint32_t *>(ctx);
}
void report_body(void *ctx,size_t bytes) {
    if(generation.load()!=*static_cast<uint32_t *>(ctx))return;
    liveBytes.store(uint32_t(bytes));livePhase.store(road_progress::Phase::downloading);
}
struct BodyTransport {
    NetworkClient &client;uint32_t gen;
    int available(){return client.available();}
    int read(uint8_t *p,size_t n){return client.read(p,n);}
    bool connected(){return client.connected();}
    bool cancelled(){return generation.load()!=gen;}
};
struct BodyClock {
    uint32_t now(){return millis();}
    void pause(){delay(1);}
};
Result fetch_one(Request *arg) {
    std::unique_ptr<Request> req(static_cast<Request *>(arg));Result r;r.gen=req->gen;
    uint32_t started=millis();
    uint8_t *input=nullptr,*expanded=nullptr;
    do {
        WiFiClient plain;
        std::unique_ptr<WiFiClientSecure> tls(req->url[4]=='s'?new WiFiClientSecure():nullptr);
        if(tls) { tls->setInsecure();tls->setHandshakeTimeout(10); }
        WiFiClient &client=tls?static_cast<WiFiClient &>(*tls):plain;
        HTTPClient http;http.setReuse(false);http.setConnectTimeout(8000);http.setTimeout(10000);
        http.setUserAgent("OrbOS/" FW_VERSION " road-maps (github.com/Ziplock78/orb-firmware)");
        const char *headers[]={"Content-Encoding","Transfer-Encoding"};http.collectHeaders(headers,2);
        http.setAcceptEncoding("gzip, identity");
        if(!http.begin(client,req->url)) {r.error="connection setup";break;}
        r.http=http.GET();
        if(r.http!=200&&r.http!=204) {r.error="tile HTTP error";http.end();break;}
        String encoding=http.header("Content-Encoding");encoding.toLowerCase();
        if(encoding.length()&&encoding!="gzip"&&encoding!="identity") {r.error="unsupported encoding";http.end();break;}
        r.encoding=encoding=="gzip"?"gzip":"identity";
        String transfer=http.header("Transfer-Encoding");transfer.toLowerCase();
        r.chunked=transfer=="chunked";
        if(transfer.length()&&transfer!="chunked"&&transfer!="identity"){r.error="unsupported transfer encoding";http.end();break;}
        int length=http.getSize();
        r.contentLength=length;
        if(generation.load()==req->gen)liveLength.store(length);
        size_t cap=0;
        if(!input_capacity(length,cap)) {r.error="tile input limit";http.end();break;}
        r.inputCapacity=cap;
        if(generation.load()==req->gen)liveCapacity.store(uint32_t(cap));
        input=static_cast<uint8_t *>(heap_caps_malloc(cap,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!input) {r.error="tile memory";http.end();break;}
        NetworkClient *stream=http.getStreamPtr();
        if(!stream&&r.http!=204){r.error="tile stream unavailable";http.end();break;}
        bool received=true;
        if(r.http!=204) {
            BodyTransport transport{*stream,req->gen};BodyClock clock;
            road_http::BodyReader<BodyTransport,BodyClock> body(transport,clock,input,cap,report_body,&req->gen);
            received=body.read(length,r.chunked);
            r.bytes=body.bytes;r.readCalls=body.readCalls;r.idlePolls=body.idlePolls;
            report_body(&req->gen,r.bytes);
            if(!received)r.error=body.error;
        }
        http.end();r.networkMs=millis()-started;
        if(!received)break;
        if(r.http==200&&r.bytes==0){r.error="empty tile body";break;}
        if(generation.load()!=req->gen) {r.error="cancelled";break;}
        Bytes decoded{input,r.bytes};
        if(encoding=="gzip"||(decoded.n>=2&&decoded.p[0]==31&&decoded.p[1]==139)) {
            livePhase.store(road_progress::Phase::unpacking);
            Bytes raw;uint32_t crc,size;
            if(!gzip_payload(decoded,raw,crc,size)) {r.error="gzip header";break;}
            expanded=static_cast<uint8_t *>(heap_caps_malloc(size?size:1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
            // ROM inflater state is sizable; keep it off the worker's stack.
            std::unique_ptr<tinfl_decompressor> inflater(new(std::nothrow) tinfl_decompressor);
            if(!expanded||!inflater) {r.error="inflate memory";break;}
            tinfl_init(inflater.get());size_t inBytes=raw.n,outBytes=size;
            tinfl_status status=tinfl_decompress(inflater.get(),raw.p,&inBytes,expanded,expanded,&outBytes,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            if(status!=TINFL_STATUS_DONE||inBytes!=raw.n||outBytes!=size||crc32(expanded,outBytes)!=crc) {r.error="gzip checksum or size";break;}
            decoded={expanded,outBytes};
            heap_caps_free(input);input=nullptr;
        }
        uint32_t decodeStart=millis();
        livePhase.store(road_progress::Phase::preparing);
        if(!decode(decoded,req->tile,req->source,nullptr,kMaxRoadCacheBytes,r.len,r.stats,poll,&req->gen)) {r.error=r.stats.error;break;}
        size_t capacity=r.len;
        r.data=static_cast<uint8_t *>(heap_caps_malloc(capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!r.data) {r.error="cache memory";break;}
        if(!decode(decoded,req->tile,req->source,r.data,capacity,r.len,r.stats,poll,&req->gen)) r.error=r.stats.error;
        r.decodeMs=millis()-decodeStart;
    } while(false);
    heap_caps_free(input);heap_caps_free(expanded);
    if(*r.error) {heap_caps_free(r.data);r.data=nullptr;r.len=0;}
    r.networkMs=r.networkMs?r.networkMs:millis()-started;
    return r; // normal return releases the request and every C++ stack object
}
void worker_loop(void *) {
    uint8_t stackProbe=0;
    workerExternalStack.store(esp_ptr_external_ram(&stackProbe));
    Serial.printf("[roads] persistent worker: stack=%lu %s\n",(unsigned long)kWorkerStackBytes,
                  workerExternalStack.load()?"PSRAM":"INTERNAL");
    for(;;) {
        Request *req=nullptr;
        if(xQueueReceive(requests,&req,portMAX_DELAY)!=pdTRUE)continue;
        Tile tile=req->tile;
        Result result=fetch_one(req);
        Serial.printf("[roads] z%lu/%lu/%lu HTTP %d bytes=%u network=%lums decode=%lums segments=%lu error=%s internal_free=%u largest=%u stack_free=%u\n",
                      (unsigned long)tile.z,(unsigned long)tile.x,(unsigned long)tile.y,result.http,unsigned(result.bytes),
                      (unsigned long)result.networkMs,(unsigned long)result.decodeMs,(unsigned long)result.stats.segments,
                      *result.error?result.error:"none",unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
                      unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
                      unsigned(uxTaskGetStackHighWaterMark(nullptr)));
        xQueueSend(results,&result,portMAX_DELAY);
        // Reuse this task and its internal stack. Never allocate another 24 KB
        // internal stack between tiles or while the UI/network heaps fragment.
    }
}
bool ensure_worker() {
    if(workerTask)return true;
    if(!requests||!results)return false;
    // TCP's initial-sequence-number hook asserts if the caller's stack is in
    // PSRAM, even though this SDK supports external task stacks in general.
    // Reserve one internal stack at startup and retain it between requests,
    // avoiding per-tile allocation after the internal heap has fragmented.
    if(xTaskCreatePinnedToCoreWithCaps(worker_loop,"vector_roads",kWorkerStackBytes,nullptr,1,&workerTask,0,
                                     MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
        workerTask=nullptr;return false;
    }
    ++workerCreates;return true;
}
void close_file() {if(file)file.close();inspecting=false;saving=false;offset=0;}
void discard() {heap_caps_free(pending.data);pending={};}
void fail(const char *why) {
    close_file();discard();error=why;state="retrying";
    if(index<count)retryAt[index]=millis()+std::min(300000u,30000u<<std::min(unsigned(attempts[index]),3u));
}
void reset() {
    generation.fetch_add(1);cleanup=true;inspecting=saving=false;discard();
    memset(done,0,sizeof(done));memset(checked,0,sizeof(checked));memset(attempts,0,sizeof(attempts));memset(retryAt,0,sizeof(retryAt));
    count=doneCount=index=0;transferred=0;lastLat=999;lastRange=-1;error="";state="idle";
}
bool endpoint_ok(const String &s) {
    if(s.length()>768)return false;
    if(s.isEmpty())return false;
    if(!s.startsWith("http://")&&!s.startsWith("https://"))return false;
    for(size_t i=0;i<s.length();++i)if(uint8_t(s[i])<=32||s[i]=='<'||s[i]=='>'||s[i]=='"')return false;
    return s.indexOf("{z}")>=0&&s.indexOf("{x}")>=0&&s.indexOf("{y}")>=0;
}
}
void prepareWorker() {
    if(!results)results=xQueueCreate(1,sizeof(Result));
    if(!requests)requests=xQueueCreate(1,sizeof(Request *));
    ensure_worker();
}
void begin(WebServer &server) {
    Preferences p;if(p.begin("roadvector",true)) {endpoint=p.getString("url",ROAD_VECTOR_URL);fetchEnabled=p.getBool("enabled",true);p.end();}
    if(!endpoint_ok(endpoint))endpoint=ROAD_VECTOR_URL;
    source=source_id(endpoint.c_str());roads_sd::set_vector_source(source);
    prepareWorker();
    server.on("/roads",HTTP_GET,[&server] {
        static const char page[] PROGMEM=R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Road maps and cache · The Orb</title>
<style>
body{background:#f5f5f4;color:#2a2622;font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:28px 20px;max-width:520px}
h1{font-size:26px;margin:0 0 4px;letter-spacing:-.02em}.sub{color:#7a7570;margin:0 0 22px}
.card{background:#fff;border:1px solid #e0e0df;border-radius:14px;padding:16px 18px;margin-bottom:14px}
h2,summary{font-size:17px;font-weight:600;margin:0}p{font-size:15px;line-height:1.5;margin:12px 0}p:last-child{margin-bottom:0}
a{color:#a65e3f;text-underline-offset:3px}a:hover{color:#2a2622}
.b{box-sizing:border-box;display:block;width:100%;padding:12px 14px;border:1px solid #e0e0df;border-radius:10px;color:#2a2622;background:#fff;text-decoration:none;margin-top:8px;font:500 15px system-ui,-apple-system,sans-serif;text-align:left;cursor:pointer}
.b:hover{border-color:#a65e3f;color:#a65e3f}.b:disabled{color:#7a7570;background:#f5f5f4;cursor:wait}
.b:focus-visible,summary:focus-visible,input:focus-visible,a:focus-visible{outline:2px solid #a65e3f;outline-offset:3px}
small{color:#7a7570;display:block;margin-top:14px;line-height:1.5}small a{color:inherit}
summary{cursor:pointer}label{display:block;font-size:15px;margin:16px 0 6px}input{box-sizing:border-box;width:100%;min-width:0;padding:12px 14px;border:1px solid #e0e0df;border-radius:10px;background:#fff;color:#2a2622;font:15px system-ui,-apple-system,sans-serif}
#cache-status{color:#7a7570;overflow-wrap:anywhere}.actions{margin-top:16px;padding-top:8px;border-top:1px solid #e0e0df}
</style></head><body>
<h1>Road maps and cache</h1><p class="sub">Nearby roads, stored on your Orb</p>
<div class="card"><h2>Road maps</h2><p>Enable Map in your Flight Tracker theme. Nearby motorways and trunk roads download automatically and stay on SD for offline use.</p>
<p>Default provider: <a href="https://openfreemap.org/">OpenFreeMap</a>. No account or API key needed.</p></div>
<div class="card"><h2>Road cache</h2><p>Clear road maps for all locations and tile sources, including older cache formats. With Map enabled and Wi-Fi connected, fresh downloads start automatically afterwards.</p>
<form method="post" action="/roads/clear"><button class="b" id="clear-cache">Clear road cache</button></form><p id="cache-status" role="status" aria-live="polite">Reading download status&hellip;</p></div>
<details class="card"><summary>Advanced tile source</summary><p>Use an OpenMapTiles XYZ URL with {z}, {x}, {y}. Saved URLs stay on this Orb and are not shown in download status.</p>
<form method="post" action="/roads/source"><label for="tile-url">Tile URL</label><input id="tile-url" type="password" name="url" placeholder="https://server/{z}/{x}/{y}.pbf"><button class="b">Save custom source</button></form>
<form class="actions" method="post" action="/roads/source"><button class="b" name="mode" value="default">Restore OpenFreeMap</button><button class="b" name="mode" value="offline">SD only</button></form></details>
<div class="card"><a class="b" href="/">Back to The Orb</a><a class="b" href="/roads.json">Download status</a></div>
<small>Map data: <a href="https://www.openstreetmap.org/copyright">OpenStreetMap contributors</a> &middot; <a href="https://openmaptiles.org/">&copy; OpenMapTiles</a>.</small>
<script>async function status(){try{const r=await fetch('/roads.json',{cache:'no-store'});if(!r.ok)throw Error();const s=await r.json();document.getElementById('clear-cache').disabled=s.cacheClearing;let text=s.status+' · '+s.saved+'/'+s.total+' tiles';if(s.cacheClearing)text='Clearing road cache: '+s.cacheDeleted+' files removed';else if(s.cacheClear==='complete')text='Road cache cleared ('+s.cacheDeleted+' files). '+text;else if(s.cacheClear==='failed')text='Cache clear failed: '+s.cacheClearError+' ('+s.cacheDeleted+' files removed). Retry Clear road cache.';else if(s.error)text+=' · '+s.error;document.getElementById('cache-status').textContent=text;}catch(e){document.getElementById('cache-status').textContent='Cannot read road status; retrying…';}setTimeout(status,1500);}status();</script></body></html>)HTML";
        server.send_P(200,"text/html; charset=utf-8",page);
    });
    server.on("/roads/clear",HTTP_POST,[&server] {
        if(clearJob.state==road_cache::ClearJob::running){server.send(409,"text/plain","Road cache is already being cleared.");return;}
        if(!sdcard::mounted()){server.send(503,"text/plain","Insert a mounted SD card before clearing road maps.");return;}
        reset();last={};saveMs=0;
        roads_sd::set_cache_clearing(true);clearJob.start();state="clearing cache";
        server.sendHeader("Location","/roads");server.send(303);
    });
    server.on("/roads/source",HTTP_POST,[&server] {
        if(clearJob.state==road_cache::ClearJob::running){server.send(409,"text/plain","Wait for the road cache to finish clearing.");return;}
        bool offline=server.arg("mode")=="offline";
        String s=server.arg("mode")=="default"?String(ROAD_VECTOR_URL):offline?endpoint:server.arg("url");
        if(!endpoint_ok(s)) {server.send(400,"text/plain","Enter an HTTP(S) XYZ template with {z}, {x}, {y}.");return;}
        Preferences p;if(!p.begin("roadvector",false)) {server.send(500,"text/plain","Cannot save source");return;}
        bool saved=p.putString("url",s)>0&&p.putBool("enabled",!offline)>0;p.end();
        if(!saved) {server.send(500,"text/plain","Cannot save source");return;}
        reset();endpoint=s;fetchEnabled=!offline;source=source_id(s.c_str());roads_sd::set_vector_source(source);
        server.sendHeader("Location","/roads");server.send(303);
    });
    server.on("/roads.json",HTTP_GET,[&server] {
        char body[1400];
        const char *clearState=clearJob.state==road_cache::ClearJob::running?"running":clearJob.state==road_cache::ClearJob::complete?"complete":clearJob.state==road_cache::ClearJob::failed?"failed":"idle";
        snprintf(body,sizeof(body),
                 "{\"status\":\"%s\",\"error\":\"%s\",\"provider\":\"%s\",\"source\":\"%08lx\","
                 "\"saved\":%u,\"total\":%u,\"http\":%d,\"bytes\":%u,\"networkMs\":%lu,\"decodeMs\":%lu,\"saveMs\":%lu,"
                 "\"segments\":%lu,\"revision\":%u,\"contentLength\":%d,\"chunked\":%s,\"contentEncoding\":\"%s\",\"readCalls\":%u,\"idlePolls\":%u,\"inputLimit\":%u,\"inputCapacity\":%u,"
                 "\"cacheClearing\":%s,\"cacheClear\":\"%s\",\"cacheDeleted\":%u,\"cacheClearError\":\"%s\","
                 "\"workerReady\":%s,\"workerStackBytes\":%lu,\"workerStackExternal\":%s,\"workerStackFree\":%u,\"workerCreates\":%u,\"workerRequests\":%u}",
                 state,error,endpoint==ROAD_VECTOR_URL?"OpenFreeMap":"custom",(unsigned long)source,
                 unsigned(doneCount),unsigned(count),last.http,unsigned(worker&&workerGeneration==generation.load()?liveBytes.load():last.bytes),(unsigned long)last.networkMs,(unsigned long)last.decodeMs,(unsigned long)saveMs,
                 (unsigned long)last.stats.segments,roads_sd::revision(),worker&&workerGeneration==generation.load()?liveLength.load():last.contentLength,last.chunked?"true":"false",last.encoding,last.readCalls,last.idlePolls,
                 unsigned(kMaxTileInputBytes),unsigned(worker&&workerGeneration==generation.load()?liveCapacity.load():last.inputCapacity),
                 clearJob.state==road_cache::ClearJob::running?"true":"false",clearState,clearJob.deleted,clearJob.error,
                 workerTask?"true":"false",(unsigned long)kWorkerStackBytes,workerExternalStack.load()?"true":"false",
                 unsigned(workerTask?uxTaskGetStackHighWaterMark(workerTask):0),workerCreates,workerRequests);
        server.send(200,"application/json",body);
    });
}
void step(double lat,double lon,double range,bool enabled,bool sdBusy) {
    bool on=enabled&&sdcard::mounted();
    if(on!=active||lat!=lastLat||lon!=lastLon||range!=lastRange) {
        reset();active=on;lastLat=lat;lastLon=lon;lastRange=range;
        if(on)count=plan(lat,lon,range,tiles);
    }
    paused=sdBusy;
    Result r;
    if(results&&xQueueReceive(results,&r,0)==pdTRUE) {
        worker=false;
        if(r.gen!=generation.load()||!active) {heap_caps_free(r.data);}
        else {
            last=r;last.data=nullptr;
            transferred+=r.bytes;
            if(!r.data) {error=r.error;state="retrying";retryAt[index]=millis()+(r.http==429?300000u:60000u);}
            else {pending=r;offset=0;state="saving";}
        }
    }
    if(paused){state="paused";return;}
    if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<8192 ||
       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<2048) {
        state="waiting for memory";return;
    }
    if(!theme_sd::try_lock())return;
    // Every card operation below is on the UI task and protected against audio.
    struct Unlock {~Unlock(){theme_sd::unlock();}} unlock;
    if(cleanup){close_file();cleanup=false;}
    if(clearJob.state==road_cache::ClearJob::running) {
        state="clearing cache";clearJob.step(clearStorage);
        if(clearJob.state==road_cache::ClearJob::complete) {
            roads_sd::set_cache_clearing(false);reset();state="cache cleared";
        } else if(clearJob.state==road_cache::ClearJob::failed) {
            state="cache clear failed";error=clearJob.error;
        }
        return;
    }
    if(clearJob.state==road_cache::ClearJob::failed){state="cache clear failed";error=clearJob.error;return;}
    if(!active){state="disabled";return;}
    if(!count){state="unsupported location or range";return;}
    if(pending.data) {
        if(!saving) {
            path(finalPath,sizeof(finalPath),tiles[index],source);snprintf(tempPath,sizeof(tempPath),"%s.tmp",finalPath);
            SD.mkdir("/roads");SD.mkdir("/roads/v2");SD.remove(tempPath);
            file=SD.open(tempPath,FILE_WRITE);if(!file){fail("cache open");return;}
            saving=true;saveStart=millis();
        }
        const size_t n=std::min(size_t(2048),pending.len-offset);
        if(file.write(pending.data+offset,n)!=n){fail("cache write");return;}offset+=n;
        if(offset==pending.len) {
            file.close();saving=false;
            // Existing files failed validation before we scheduled this request.
            SD.remove(finalPath);
            if(!SD.rename(tempPath,finalPath)){fail("cache publish");return;}
            saveMs=millis()-saveStart;discard();done[index]=true;++doneCount;error="";
            roads_sd::published();state="ready";
        }
        return;
    }
    if(worker){state="downloading";return;}
    if(inspecting) {
        uint8_t b[2048];size_t n=std::min(size_t(2048),size_t(file.size())-offset);
        if(file.read(b,n)!=n){close_file();return;}
        for(size_t i=0;i<n;i+=2)if((unsigned(b[i])|(unsigned(b[i+1])<<8))>EXTENT){close_file();return;}
        checkCrc=crc_update(checkCrc,b,n);offset+=n;
        if(offset==file.size()) {
            bool good=~checkCrc==expectedCrc;close_file();
            if(good){done[index]=true;++doneCount;state="cached";return;}
        } else return;
        // A failed inspection falls through to replacing this tile.
    } else {
        bool found=false;
        for(size_t i=0;i<count;++i)if(!done[i]&&attempts[i]<3&&(!retryAt[i]||int32_t(millis()-retryAt[i])>=0)){index=i;found=true;break;}
        if(!found){
            bool exhausted=true;for(size_t i=0;i<count;++i)if(!done[i]&&attempts[i]<3)exhausted=false;
            state=doneCount==count?"ready":exhausted?"failed":"retrying";return;
        }
        path(finalPath,sizeof(finalPath),tiles[index],source);
        file=checked[index]?File():SD.open(finalPath,FILE_READ);checked[index]=true;
        if(file) {
            uint8_t h[HEADER];
            if(file.read(h,HEADER)==HEADER&&cache_header(h,file.size(),tiles[index],source)) {
                offset=HEADER;checkCrc=~0u;expectedCrc=u32(h+28);inspecting=true;state="checking cache";return;
            }
            close_file();
        }
    }
    if(!fetchEnabled){state="SD only";retryAt[index]=millis()+60000;return;}
    if(transferred>50000000u-kMaxTileInputBytes){state="download budget reached";return;}
    if(WiFi.status()!=WL_CONNECTED){state="waiting for Wi-Fi";return;}
    if(!results||!requests){state="unavailable";error="worker queue memory";return;}
    if(int32_t(millis()-requestAt)<0)return;
    if(!ensure_worker()){state="waiting for memory";error="worker internal stack memory";requestAt=millis()+30000;return;}
    String url=endpoint;url.replace("{z}",String(tiles[index].z));url.replace("{x}",String(tiles[index].x));url.replace("{y}",String(tiles[index].y));
    Request *req=new(std::nothrow) Request;
    if(!req){error="worker memory";requestAt=millis()+30000;return;}
    req->tile=tiles[index];req->source=source;req->gen=generation.load();url.toCharArray(req->url,sizeof(req->url));
    requestAt=millis()+1000;
    workerStarted=millis();workerGeneration=req->gen;liveBytes.store(0);liveCapacity.store(0);liveLength.store(-1);livePhase.store(road_progress::Phase::waiting);
    worker=xQueueSend(requests,&req,0)==pdTRUE;
    if(!worker){delete req;error="worker request queue busy";requestAt=millis()+1000;}
    else {++attempts[index];++workerRequests;state="downloading";error="";}
}
void progressText(char *out,size_t capacity) {
    using road_progress::Phase;
    road_progress::Snapshot s;s.saved=unsigned(doneCount);s.total=unsigned(count);s.deleted=clearJob.deleted;
    if(clearJob.state==road_cache::ClearJob::running)s.phase=Phase::clearing;
    else if(clearJob.state==road_cache::ClearJob::failed)s.phase=Phase::clearFailed;
    else if(!sdcard::mounted())s.phase=Phase::needSd;
    else if(!fetchEnabled||!active||!count||doneCount==count)s.phase=Phase::none;
    else if(paused)s.phase=Phase::paused;
    else if(worker) {
        s.phase=workerGeneration==generation.load()?livePhase.load():Phase::restarting;
        s.bytes=liveBytes.load();s.length=liveLength.load();s.seconds=(millis()-workerStarted)/1000;
    } else if(pending.data||saving)s.phase=Phase::saving;
    else if(inspecting)s.phase=Phase::checking;
    else if(WiFi.status()!=WL_CONNECTED)s.phase=Phase::needWifi;
    else if(!strcmp(state,"failed")||!strcmp(state,"download budget reached"))s.phase=Phase::failed;
    else {
        uint32_t wait=UINT32_MAX;
        for(size_t i=0;i<count;++i)if(!done[i]&&attempts[i]<3) {
            int32_t remaining=retryAt[i]?int32_t(retryAt[i]-millis()):0;
            wait=std::min(wait,remaining>0?uint32_t(remaining):uint32_t(0));
        }
        if(wait!=UINT32_MAX&&wait>0){s.phase=Phase::retrying;s.seconds=(wait+999)/1000;}
        else s.phase=Phase::checking;
    }
    road_progress::format(s,out,capacity);
}
} // namespace road_vector_download
#endif
