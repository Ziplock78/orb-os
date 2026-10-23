#include "road_cache_clear.h"
#include "road_http_body.h"
// Match the ESP32 SDK's late POSIX header: the old MAX_INPUT identifier
// silently became 255 at download call sites even though its declaration was 512K.
#ifndef MAX_INPUT
#define MAX_INPUT 255
#endif
#include "road_vector.h"
#include "road_progress.h"
#include <assert.h>
#include <string.h>
#include <string>
#include <vector>
#include <iostream>

struct Clock {
    uint32_t ms=0,increment=1;
    uint32_t now(){return ms;}
    void pause(){ms+=increment;}
};
struct Transport {
    Clock &clock;std::string data;size_t pos=0;
    uint32_t readyAt=0,readAdvance=0;int temporaryReads=0;
    bool holdOpen=false,cancel=false;size_t batch=7;
    int available(){return clock.ms<readyAt?0:int(data.size()-pos);}
    int read(uint8_t *p,size_t n){
        if(temporaryReads>0)return --temporaryReads%2?-1:0;
        size_t got=std::min(std::min(n,batch),data.size()-pos);
        memcpy(p,data.data()+pos,got);pos+=got;clock.ms+=readAdvance;return int(got);
    }
    bool connected(){return holdOpen||pos<data.size()||clock.ms<readyAt;}
    bool cancelled(){return cancel;}
};
static void transport_tests(){
    uint8_t out[32];
    {
        static_assert(road_vector::kMaxTileInputBytes==512*1024,"terminal macro must not change tile budget");
        size_t cap=0;assert(road_vector::input_capacity(24211,cap)&&cap==24211);
        Clock c;Transport t{c,std::string(24211,'x')};t.batch=1024;
        std::vector<uint8_t> buffer(cap);std::vector<size_t> reports;
        auto report=[](void *ctx,size_t n){static_cast<std::vector<size_t> *>(ctx)->push_back(n);};
        road_http::BodyReader<Transport,Clock> body(t,c,buffer.data(),cap,report,&reports);
        assert(body.read(24211,false)&&body.bytes==24211&&body.readCalls>0);
        assert(!memcmp(buffer.data(),t.data.data(),24211)&&reports.front()==1024&&reports.back()==23552);
        assert(road_vector::input_capacity(-1,cap)&&cap==524288);
        assert(road_vector::input_capacity(524288,cap)&&cap==524288);
        assert(!road_vector::input_capacity(524289,cap)&&cap==0);
    }
    {
        Clock c;Transport t{c,"abcdef"};t.readyAt=5;t.temporaryReads=3;
        road_http::BodyReader<Transport,Clock> body(t,c,out,sizeof(out));
        assert(body.read(6,false)&&body.bytes==6&&!memcmp(out,"abcdef",6));
        assert(body.idlePolls>=8&&body.readCalls>=4); // TLS no-data is retried, never EOF
    }
    {
        Clock c;Transport t{c,"3;ext=value\r\nabc\r\n2\r\nde\r\n0\r\nX-Test: yes\r\n\r\n"};
        road_http::BodyReader<Transport,Clock> body(t,c,out,sizeof(out));
        assert(body.read(-1,true)&&body.bytes==5&&!memcmp(out,"abcde",5));
    }
    {
        Clock c;Transport t{c,"xyz"};
        road_http::BodyReader<Transport,Clock> body(t,c,out,sizeof(out));
        assert(!body.read(5,false)&&body.bytes==3&&strstr(body.error,"closed"));
    }
    {
        Clock c;Transport t{c,"xyz"};
        road_http::BodyReader<Transport,Clock> body(t,c,out,sizeof(out));
        assert(body.read(-1,false)&&body.bytes==3);
    }
    {
        Clock c;Transport t{c,"abcde"};
        road_http::BodyReader<Transport,Clock> body(t,c,out,4);
        assert(!body.read(-1,false)&&body.bytes==4&&strstr(body.error,"limit"));
    }
    {
        Clock c;Transport t{c,"abc"};
        road_http::BodyReader<Transport,Clock> body(t,c,out,2);
        assert(!body.read(3,false)&&body.readCalls==0);
    }
    {
        Clock c;c.increment=100;Transport t{c,""};t.holdOpen=true;
        road_http::BodyReader<Transport,Clock> body(t,c,out,sizeof(out));
        assert(!body.read(1,false)&&!strcmp(body.error,"tile body idle timeout")&&c.ms==10000);
    }
    {
        Clock c;Transport t{c,"abc"};t.cancel=true;
        road_http::BodyReader<Transport,Clock> body(t,c,out,sizeof(out));
        assert(!body.read(3,false)&&!strcmp(body.error,"cancelled")&&body.bytes==0);
    }
    {
        Clock c;Transport t{c,std::string(100,'a')};t.batch=1;t.readAdvance=1000;
        uint8_t bigger[128];road_http::BodyReader<Transport,Clock> body(t,c,bigger,sizeof(bigger));
        assert(!body.read(100,false)&&!strcmp(body.error,"tile body total timeout")&&c.ms==30000);
    }
    for(const char *s:{"g\r\n","ffffffffffffffff\r\n","2\r\na","1\r\naXX","1\na\n","0\r\nX-Test: unfinished"}) {
        Clock c;Transport t{c,s};road_http::BodyReader<Transport,Clock> body(t,c,out,sizeof(out));
        assert(!body.read(-1,true)&&*body.error);
    }
    {
        Clock c;Transport t{c,"0\r\n\r\n"};road_http::BodyReader<Transport,Clock> body(t,c,out,0);
        assert(body.read(-1,true)&&body.bytes==0);
    }
    {
        Clock c;Transport t{c,std::string(129,'1')+"\r\n"};
        road_http::BodyReader<Transport,Clock> body(t,c,out,sizeof(out));
        assert(!body.read(-1,true)&&strstr(body.error,"header limit"));
    }
}
struct Entry {std::string dir,name;bool directory=false,removed=false;};
struct Storage {
    std::vector<Entry> entries;std::string current,failPath;size_t nextIndex=0;
    bool absent=false;unsigned removes=0,closes=0;
    road_cache::Scan open(const char *dir){
        assert(!strcmp(dir,"/roads")||!strcmp(dir,"/roads/v2"));
        current=dir;nextIndex=0;return absent?road_cache::Scan::missing:road_cache::Scan::entry;
    }
    road_cache::Scan next(char *name,size_t cap,bool &directory){
        while(nextIndex<entries.size()){
            Entry &e=entries[nextIndex++];if(e.dir!=current||e.removed)continue;
            assert(e.name.size()<cap);memcpy(name,e.name.c_str(),e.name.size()+1);directory=e.directory;return road_cache::Scan::entry;
        }
        return road_cache::Scan::end;
    }
    bool remove(const char *path){
        if(failPath==path)return false;
        for(auto &e:entries)if(e.dir+"/"+e.name==path){assert(!e.directory&&!e.removed);e.removed=true;++removes;return true;}
        assert(false);return false;
    }
    void close(){++closes;current.clear();}
};
static void clear_tests(){
    using Job=road_cache::ClearJob;
    Storage s;
    s.entries={{"/roads","r53_-2.bin"},{"/roads","q212_-7_m.bin"},{"/roads","d53_-2_t.bin"},
               {"/roads","_download.pts"},{"/roads","_download.part"},{"/roads","v2",true},
               {"/roads/v2","ffeac602_11_1010_681.bin"},{"/roads/v2","ffeac602_11_1010_681.bin.tmp"},
               {"/roads/v2","OTHER.BIN"},{"/roads","readme.txt"},{"/roads/v2","notes.txt"},
               {"/roads","../themes/clock.bin"},{"/roads/v2","nested",true},{"/themes","clock.bin"}};
    Job job;job.start();
    for(int i=0;i<100&&job.state==Job::running;++i){unsigned before=s.removes;job.step(s);assert(s.removes-before<=1);}
    assert(job.state==Job::complete&&job.deleted==8&&s.removes==8);
    for(size_t i=9;i<s.entries.size();++i)assert(!s.entries[i].removed);
    job.start();for(int i=0;i<100&&job.state==Job::running;++i)job.step(s);
    assert(job.state==Job::complete&&job.deleted==0); // already empty
    Storage missing;missing.absent=true;job.start();job.step(missing);job.step(missing);
    assert(job.state==Job::complete&&job.deleted==0);
    Storage failed;failed.entries={{"/roads","r53_-2.bin"}};failed.failPath="/roads/r53_-2.bin";
    job.start();job.step(failed);job.step(failed);assert(job.state==Job::failed&&*job.error&&job.deleted==0);
    job.step(failed);assert(failed.removes==0); // no retries or download resumption hidden in failure
    failed.failPath.clear();job.start();for(int i=0;i<20&&job.state==Job::running;++i)job.step(failed);
    assert(job.state==Job::complete&&job.deleted==1);
}
static void progress_tests(){
    using namespace road_progress;
    Snapshot s;s.saved=1;s.total=4;char out[144];
    format(s,out,sizeof(out));assert(!*out);
    s.phase=Phase::waiting;s.seconds=3;format(s,out,sizeof(out));assert(strstr(out,"3s")&&strstr(out,"1/4 tiles saved"));
    s.phase=Phase::downloading;s.bytes=8192;s.length=24211;format(s,out,sizeof(out));assert(strstr(out,"8/24 KB"));
    s.phase=Phase::saving;format(s,out,sizeof(out));assert(strstr(out,"Saving roads"));
    s.phase=Phase::retrying;s.seconds=60;format(s,out,sizeof(out));assert(strstr(out,"60s"));
    s.phase=Phase::clearing;s.deleted=117;format(s,out,sizeof(out));assert(strstr(out,"117 files")&&!strstr(out,"tiles saved"));
    s.phase=Phase::failed;format(s,out,sizeof(out));assert(strstr(out,"failed"));
    char small[4];format(s,small,sizeof(small));assert(small[3]==0);
}
int main(){transport_tests();clear_tests();progress_tests();std::cout<<"road cache clear, input limit, HTTP body and progress tests passed\n";}
