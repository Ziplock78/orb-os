#!/usr/bin/env python3
"""Test the actual embedded browser page and compare JS tiles with the C++ generator.

Uses headless Edge/Chrome and a local fake Orb/map service. No device is flashed,
no public map requests are made. Run tools/test_road_tiles.py first to build the
portable reference generator. --browser can select another Chromium executable.
"""
import argparse
from email.parser import BytesParser
from email.policy import default
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import subprocess
import threading
import time
import urllib.parse

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.pio/road-tests'


def embedded(name, delimiter):
    return (ROOT / 'src' / name).read_text(encoding='utf-8').split('R"'+delimiter+'(', 1)[1].rsplit(')'+delimiter+'";', 1)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', default=r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe')
    parser.add_argument('--live', action='store_true', help='Also fetch one real Provo quarter-cell directly in the browser')
    args = parser.parse_args()
    fixtures = []
    synthetic = {'elements': [
        {'type': 'way', 'geometry': [{'lat': y, 'lon': x} for y, x in points]}
        for points in [
            [(40.01, -111.9), (40.1, -111.9), (40.2, -111.9), (40.2, -111.8)],
            [(39.9, -111.9), (40.1, -111.8), (40.4, -111.7)],
            [(40.1, -112.2), (40.15, -111.9), (40.2, -112.2), (40.22, -111.8)],
            [(40.125, -111.875), (40.2, -111.875)],
            [(40.1, -111.9)] * 2048,
        ]
    ]}
    p = BUILD / 'browser-synthetic.json'; p.write_text(json.dumps(synthetic))
    candidates = [(p, 160, -448, 1)]
    for name, lat, lon, span in [
        ('overpass-40.0_-111.75-motorway.json',160,-447,1),
        ('overpass-40.0_-111.75-trunk.json',160,-447,1),
        ('overpass-40_-112-motorway.json',160,-448,4),
    ]:
        fixture = BUILD / name
        if fixture.exists(): candidates.append((fixture,lat,lon,span))
    for i, (fixture, lat, lon, span) in enumerate(candidates):
        binary = BUILD / f'browser-reference-{i}.bin'
        subprocess.run([str(BUILD / 'test.exe'),str(fixture),str(lat),str(lon),str(span),str(binary)],check=True)
        fixtures.append({'doc': json.loads(fixture.read_bytes()), 'expected': list(binary.read_bytes()),
                         'cell': {'lat':lat,'lon':lon,'span':span,'roadClass':'trunk' if '-trunk' in fixture.name else 'motorway'}})
    page = embedded('road_preload_page.h','HTML')
    processor = embedded('road_preload_js.h','JS')
    state = {'uploads':set(), 'mapRequests':0, 'release':0, 'uploadAttempts':0, 'planLocations':0, 'errors':[], 'mode':'normal'}
    live_result = {}
    ui_cases = fixtures[1:3] if len(fixtures)>=3 else [fixtures[0],fixtures[0]]
    harness = r"""
<script>
const nativeFetch=window.fetch.bind(window);
window.fetch=(input,options)=>nativeFetch(String(input).startsWith('https://')?'/overpass':input,options);
(async()=>{
 const wait=async(predicate)=>{for(let i=0;i<90000;++i){if(predicate())return;await new Promise(r=>setTimeout(r,20));}throw Error('UI wait timed out: '+document.getElementById('status').textContent);};
 const assert=(value,message)=>{if(!value)throw Error(message);};
 try{
  const fixtures=await(await nativeFetch('/fixtures')).json();
  for(const item of fixtures){
   const result=new Uint8Array(await RoadTiles.process(item.doc,item.cell));
   const different=result.findIndex((v,i)=>v!==item.expected[i]);
   assert(result.length===item.expected.length&&different<0,'C++/JS binary mismatch: case '+fixtures.indexOf(item)+' byte '+different+' JS '+Array.from(result).slice(Math.max(0,different-4),different+8)+' C++ '+item.expected.slice(Math.max(0,different-4),different+8)+' lengths '+result.length+'/'+item.expected.length);
  }
  for(const doc of [{remark:'timeout',elements:[]},{remark:'',elements:[]},{elements:[{type:'way',geometry:[null]}]}, {elements:[{type:'way',geometry:[{lat:91,lon:0}]}]}]){
   let rejected=false;try{await RoadTiles.process(doc,fixtures[0].cell);}catch(e){rejected=true;}assert(rejected,'Invalid response accepted');
  }
  assert(new Uint8Array(await RoadTiles.process({elements:[]},fixtures[0].cell)).length===6,'Empty tile rejected');
  await wait(()=>!document.getElementById('start').disabled);
  assert(document.querySelectorAll('input[data-location]').length===2,'Missing saved locations');
  assert(!document.querySelector('#locations img'),'Location name interpreted as HTML');
  document.querySelector('input[data-location="1"]').checked=true;
  document.getElementById('start').click();
  await wait(()=>!document.getElementById('start').disabled);
  assert(document.getElementById('status').textContent.includes('Maps ready'),'Preload failed: '+document.getElementById('log').textContent);
  assert(document.getElementById('summary').textContent==='2 uploaded · 1 already cached · 0 pending','Progress counts wrong');
  document.getElementById('start').click();
  await wait(()=>!document.getElementById('start').disabled);
  assert(document.getElementById('summary').textContent==='0 uploaded · 3 already cached · 0 pending','Cached tiles fetched again');
  const state=await(await nativeFetch('/state')).json();
  assert(state.mapRequests===2&&state.uploads.length===2&&state.uploadAttempts===3&&state.planLocations===2&&state.release===2&&!state.errors.length,JSON.stringify(state));
  await nativeFetch('/test/reset',{method:'POST',body:'fail'});
  document.getElementById('start').click();
  await wait(()=>document.getElementById('status').textContent.includes('Retrying in'));
  assert(document.getElementById('start').disabled,'Retry ended the run prematurely');
  assert(document.getElementById('summary').textContent==='1 uploaded · 1 already cached · 1 pending','Failed section lost progress');
  assert(document.getElementById('progress').value===2,'Failed section counted as completed');
  await wait(()=>!document.getElementById('start').disabled);
  assert(document.getElementById('summary').textContent==='2 uploaded · 1 already cached · 0 pending','Failed section did not retry automatically');
  const recovered=await(await nativeFetch('/state')).json();
  assert(recovered.mapRequests===5&&recovered.release===3,'Completed sections redownloaded or session released during retry');
  if(fixtures.length>=4){
   await nativeFetch('/test/reset',{method:'POST',body:'cover'});
   document.getElementById('start').click();await wait(()=>!document.getElementById('start').disabled);
   assert(document.getElementById('summary').textContent==='1 uploaded · 2 already cached · 0 pending','Full-cell coverage did not clear the failed starter');
   assert(document.getElementById('log').textContent.includes('Covered '),'Missing starter coverage result');
   const covered=await(await nativeFetch('/state')).json();
   assert(covered.mapRequests===7,'Covered starter downloaded again');
  }
  await nativeFetch('/test/reset',{method:'POST',body:'normal'});
  const rateFetch=window.fetch, rateCalls=[], expectedPauses=[90000,180000,240000,300000,300000];
  window.fetch=(input,options)=>{
   if(!String(input).startsWith('https://'))return rateFetch(input,options);
   rateCalls.push(Date.now());
   const n=rateCalls.length;
   if(n===7)assert(rateLimitStreak===0,'Successful response did not reset rate-limit backoff');
   if(n<=5||n===7){
    const headers={};
    if(n===1)headers['Retry-After']='90';
    if(n===2)headers['Retry-After']=new Date(Date.now()+180000).toUTCString();
    if(n===3)headers['Retry-After']='invalid';
    if(n===4)headers['Retry-After']='0';
    return Promise.resolve(new Response('Too many requests',{status:429,headers}));
   }
   return rateFetch(input,options);
  };
  document.getElementById('start').click();
  await wait(()=>document.getElementById('status').textContent.includes('rate limited requests'));
  assert(document.getElementById('log').textContent.includes('retry in 90s'),'429 log does not reflect Retry-After');
  await wait(()=>!document.getElementById('start').disabled);
  assert(rateCalls.length===8,'429 recovery failed');
  expectedPauses.forEach((pause,i)=>assert(rateCalls[i+1]-rateCalls[i]>=pause-1000,'Map request started during rate-limit cooldown'));
  assert(document.getElementById('summary').textContent==='2 uploaded · 1 already cached · 0 pending','429 lost completed sections');
  window.fetch=rateFetch;
  await nativeFetch('/test/reset',{method:'POST',body:'normal'});
  window.fetch=(input,options)=>String(input).startsWith('https://')?Promise.resolve(new Response('',{status:429})):rateFetch(input,options);
  document.getElementById('start').click();
  await wait(()=>document.getElementById('status').textContent.includes('rate limited requests'));
  document.getElementById('stop').click();await wait(()=>!document.getElementById('start').disabled);
  assert(document.getElementById('status').textContent.startsWith('Stopped.'),'Stop failed during rate-limit cooldown');
  window.fetch=rateFetch;
  await nativeFetch('/test/reset',{method:'POST',body:'always-fail'});
  document.getElementById('start').click();
  await wait(()=>document.getElementById('log').textContent.includes('attempt 7'));
  const retryLog=document.getElementById('log').textContent;
  assert([30,60,120,240,300].every(seconds=>retryLog.includes('retry in '+seconds+'s')),'Missing increasing retry backoff');
  assert((retryLog.match(/retry in 300s/g)||[]).length>=3&&!retryLog.includes('retry in 600s'),'Retries did not continue at the five-minute cap');
  assert(document.getElementById('progress').value===1,'Failures filled the progress bar');
  document.getElementById('stop').click();await wait(()=>!document.getElementById('start').disabled);
  assert(document.getElementById('status').textContent.startsWith('Stopped.'),'Stop failed during retry backoff');
  await nativeFetch('/test/reset',{method:'POST',body:'normal'});
  const normalFetch=window.fetch;
  window.fetch=(input,options)=>String(input).startsWith('https://')?new Promise((resolve,reject)=>{
    options.signal.addEventListener('abort',()=>reject(new DOMException('Stopped','AbortError')));
    setTimeout(()=>document.getElementById('stop').click(),0);
  }):normalFetch(input,options);
  document.getElementById('start').click();await wait(()=>!document.getElementById('start').disabled);
  assert(document.getElementById('status').textContent.startsWith('Stopped.'),'Stop did not release the session');
  window.fetch=normalFetch;
  // LIVE_TEST
  document.body.dataset.testResult='pass';
  const result=document.createElement('pre');result.id='test-result';result.textContent='PASS: '+fixtures.length+' binary comparisons; browser uploads, automatic recovery, 429 cooldowns, Retry-After seconds/date, capped backoff, progress, stop and release';document.body.append(result);
 }catch(e){document.body.dataset.testResult='fail';const result=document.createElement('pre');result.id='test-result';result.textContent=String(e.stack||e);document.body.append(result);}
})();
</script>
"""
    if args.live:
        endpoint = re.search(r'#define ROAD_OVERPASS_URL "([^"]+)"',(ROOT/'src/config.h').read_text())[1]
        live_script = r"""
        await nativeFetch('/live-start',{method:'POST'});
        const cell={lat:160,lon:-447,span:1,roadClass:'motorway'};
        const response=await nativeFetch(LIVE_ENDPOINT,{method:'POST',headers:{'Content-Type':'text/plain'},body:RoadTiles.query(cell),credentials:'omit'});
        assert(response.ok,'Live map service HTTP '+response.status);
        const text=await response.text(), binary=new Uint8Array(await RoadTiles.process(JSON.parse(text),cell));
        await nativeFetch('/live-result',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text,binary:Array.from(binary),cell})});
        """.replace('LIVE_ENDPOINT',json.dumps(endpoint))
        harness=harness.replace('// LIVE_TEST',live_script)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass

        def reply(self, value, mime='application/json', code=200):
            body = value.encode() if isinstance(value,str) else json.dumps(value).encode()
            self.send_response(code); self.send_header('Content-Type',mime)
            self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)

        def do_GET(self):
            route = urllib.parse.urlparse(self.path).path
            if route=='/': self.reply(page.replace('</body>',harness+'</body>'),'text/html')
            elif route=='/roads/processor.js': self.reply(processor,'application/javascript')
            elif route=='/fixtures': self.reply(fixtures)
            elif route=='/state': self.reply({**state,'uploads':list(state['uploads'])})
            elif route=='/roads/locations.json': self.reply({'sdMounted':True,'rangeKm':58,'endpoint':'https://maps.example.test/interpreter','locations':[{'name':'Provo','lat':40.23,'lon':-111.66},{'name':'<img src=x>Heber','lat':40.5,'lon':-111.4}]})
            else: self.reply('Not found','text/plain',404)

        def do_POST(self):
            route = urllib.parse.urlparse(self.path).path
            body = self.rfile.read(int(self.headers.get('Content-Length',0)))
            if route=='/roads/session': self.reply({'token':'test-token','ready':True})
            elif route=='/roads/release': state['release']+=1; self.reply('ok','text/plain')
            elif route=='/roads/plan':
                state['planLocations']=len(json.loads(body)['locations'])
                cases=[ui_cases[0],fixtures[3]] if state['mode']=='cover' else ui_cases
                self.reply({'tiles':[{'id':0,'cached':True,'path':'/roads/already.bin'},*[{'id':i+1,'cached':i+1 in state['uploads'],'path':f'/roads/test{i}.bin',**case['cell']} for i,case in enumerate(cases)]]})
            elif route=='/overpass':
                state['mapRequests']+=1
                which=1 if b'"trunk"' in body else 0
                if state['mode']=='cover':
                    if b'(40,-112,41,-111)' in body: self.reply(fixtures[3]['doc'])
                    else: self.reply('Server busy','text/plain',504)
                elif state['mode']=='always-fail': self.reply('Server busy','text/plain',504)
                elif state['mode']=='fail' and which==0:
                    state['mode']='normal';self.reply('Server busy','text/plain',504)
                else: self.reply(ui_cases[which]['doc'])
            elif route=='/test/reset':
                state['uploads'].clear();state['mode']=body.decode();self.reply('ok','text/plain')
            elif route=='/live-start': live_result['started']=time.monotonic();self.reply('ok','text/plain')
            elif route=='/live-result':
                live_result.update(json.loads(body));live_result['seconds']=time.monotonic()-live_result['started'];self.reply('ok','text/plain')
            elif route=='/roads/tile':
                state['uploadAttempts']+=1
                if state['uploadAttempts']==1: self.reply('SD card busy','text/plain',503);return
                tile_id=int(urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)['id'][0])
                message=BytesParser(policy=default).parsebytes(('Content-Type: '+self.headers['Content-Type']+'\r\nMIME-Version: 1.0\r\n\r\n').encode()+body)
                data=list(message.iter_parts())[0].get_payload(decode=True)
                expected=fixtures[3] if state['mode']=='cover' and tile_id==2 else ui_cases[tile_id-1]
                if data != bytes(expected['expected']): state['errors'].append('Uploaded binary mismatch')
                state['uploads'].add(tile_id);self.reply('ok','text/plain')
            else: self.reply('Not found','text/plain',404)

    server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        result=subprocess.run([args.browser,'--headless','--disable-gpu','--no-first-run',
            '--user-data-dir='+str(BUILD/'browser-profile'),'--dump-dom','--virtual-time-budget=3600000',
            f'http://127.0.0.1:{server.server_port}/'],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=90,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        (BUILD/'browser-result.html').write_text(result.stdout,encoding='utf-8')
        match=re.search(r'<pre id="test-result">(.*?)</pre>',result.stdout,re.S)
        print(match.group(1) if match else result.stderr[-1500:])
        if 'data-test-result="pass"' not in result.stdout: raise SystemExit('Browser tests failed; see .pio/road-tests/browser-result.html')
        if args.live:
            fixture=BUILD/'browser-live.json';fixture.write_text(live_result['text'])
            reference=BUILD/'browser-live-reference.bin'
            subprocess.run([str(BUILD/'test.exe'),str(fixture),'160','-447','1',str(reference)],check=True)
            assert reference.read_bytes()==bytes(live_result['binary']), 'Live C++/JS binary mismatch'
            print(f"Live browser fetch/process: {len(live_result['text'].encode())} response bytes -> {reference.stat().st_size} tile bytes in {live_result['seconds']:.2f}s; C++ binary matches")
    finally: server.shutdown();server.server_close()


if __name__=='__main__': main()
