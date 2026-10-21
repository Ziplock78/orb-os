#pragma once
static const char ROAD_PRELOAD_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Preload road maps · The Orb</title>
<style>
*{box-sizing:border-box}body{background:#f5f5f4;color:#2a2622;font:16px system-ui;margin:0 auto;padding:28px 20px;max-width:650px}
h1{font-size:26px;margin:20px 0 8px}p{color:#716b65;line-height:1.5}.card{background:white;border:1px solid #dedbd7;border-radius:14px;padding:18px;margin:16px 0}
label.location{display:flex;gap:12px;padding:10px 0;align-items:center}input[type=checkbox]{width:20px;height:20px;accent-color:#a65e3f}small{display:block;color:#716b65;margin-top:3px}button{font:inherit;border:1px solid #c5b9af;border-radius:9px;padding:11px 17px;background:white;cursor:pointer}button.primary{background:#985638;color:white;border-color:#985638}button:disabled{opacity:.5;cursor:default}
input[type=url]{font:14px system-ui;width:100%;padding:9px;margin-top:10px}a{color:#985638}progress{width:100%;height:18px;margin:14px 0}#status{white-space:pre-line;line-height:1.5}#log{font:13px ui-monospace,monospace;max-height:230px;overflow:auto;white-space:pre-wrap;overflow-wrap:anywhere}details{margin-top:16px}#summary{margin-bottom:16px}
</style></head><body><a href="/">← The Orb</a><h1>Preload road maps</h1>
<p>Prepare maps for your saved locations using this browser. Completed maps stay on the Orb’s SD card. Keep this page open until it finishes.</p>
<div class="card"><strong>Locations</strong><div id="locations">Loading locations…</div><p id="range"></p>
<details><summary>Map service</summary><label>Download endpoint<input id="endpoint" type="url" aria-label="Map download endpoint"></label><small>The service must allow browser requests. Requests run one at a time.</small></details></div>
<button id="start" class="primary" disabled>Preload selected maps</button> <button id="stop" disabled>Stop</button>
<div class="card" role="status" aria-live="polite"><div id="status">Checking the Orb…</div><progress id="progress" value="0" max="1"></progress><div id="summary"></div><div id="log"></div></div>
<script src="/roads/processor.js"></script><script>
'use strict';
const $ = id => document.getElementById(id), sleep = ms => new Promise(r=>setTimeout(r,ms));
let config, token='', stopped=false, controller, heartbeat, leaseError='', nextRequest=0, uploading=false;
let rateLimitUntil=0, rateLimitStreak=0;
const say = s => $('status').textContent=s;
const logLines=[];
const log = s => { logLines.push(s);if(logLines.length>200)logLines.shift();$('log').textContent=logLines.join('\n');$('log').scrollTop=$('log').scrollHeight; };
const url = path => path+'?token='+encodeURIComponent(token);
async function local(path, options={}) {
  const r = await fetch(path,{...options,signal:AbortSignal.timeout(path.startsWith('/roads/tile')?90000:15000)});
  if (!r.ok) throw new Error((await r.text()).slice(0,200));
  return r;
}
async function acquire() {
  let r = await local('/roads/session',{method:'POST'}), session = await r.json();
  token = session.token;
  heartbeat = setInterval(async()=>{
    if(uploading)return;
    try { await local(url('/roads/session'),{method:'POST'}); }
    catch(e) { if(!uploading){leaseError = e.message; controller?.abort();} }
  },10000);
  const deadline = Date.now()+90000;
  while (!session.ready) {
    check(); if (Date.now()>deadline) throw new Error('Device download did not stop in time. Try again shortly.');
    say('Waiting for the Orb to finish its current request…'); await sleep(1000);
    session = await (await local(url('/roads/session'),{method:'POST'})).json();
  }
}
function check() { if (leaseError) throw new Error(leaseError); if (stopped) throw new Error('Stopped. Completed maps are saved.'); }
async function download(tile, endpoint) {
  while (Date.now()<nextRequest) { check(); say('Waiting before the next map request: '+Math.ceil((nextRequest-Date.now())/1000)+'s…'); await sleep(Math.min(500,nextRequest-Date.now())); }
  check(); controller = new AbortController();
  const timeout = setTimeout(()=>controller.abort(),150000);
  nextRequest = Date.now()+10000;
  try {
    say('Waiting for map service: '+tile.roadClass+'\n'+tile.path);
    const r = await fetch(endpoint,{method:'POST',headers:{'Content-Type':'text/plain'},body:RoadTiles.query(tile),signal:controller.signal,credentials:'omit'});
    if (!r.ok) {
      const error=new Error('Map service HTTP '+r.status+(r.status===429?' (rate limited)':''));
      if(r.status===429){
        const now=Date.now(), header=r.headers.get('Retry-After');
        const requested=header&&/^\d+$/.test(header.trim())?now+Number(header)*1000:Date.parse(header||'');
        const fallback=Math.min(300000,60000*2**Math.min(rateLimitStreak++,3));
        rateLimitUntil=Math.max(now+fallback,Number.isFinite(requested)?requested:0);
        nextRequest=Math.max(nextRequest,rateLimitUntil);error.retryAt=rateLimitUntil;
      }
      throw error;
    }
    rateLimitStreak=0;
    const reader = r.body.getReader(), decoder = new TextDecoder(); let text='', bytes=0;
    while (true) {
      check(); const part = await reader.read(); if (part.done) break;
      bytes += part.value.byteLength;
      if (bytes>8*1024*1024) { await reader.cancel(); throw new Error('Map response exceeds 8 MB'); }
      text += decoder.decode(part.value,{stream:true});
      say('Downloading '+tile.roadClass+': '+Math.ceil(bytes/1024)+' KB\n'+tile.path);
    }
    text += decoder.decode(); return JSON.parse(text);
  } catch(e) {
    if (stopped || leaseError) check();
    if (e instanceof TypeError) throw new Error('Browser could not reach the map service. Check the connection or try an endpoint that allows browser requests.');
    throw e;
  } finally { clearTimeout(timeout); controller=null; }
}
$('stop').onclick=()=>{stopped=true;controller?.abort();say('Stopping…');};
window.addEventListener('pagehide',()=>{if(token) navigator.sendBeacon(url('/roads/release'));});
$('start').onclick=async()=>{
  const locations=[...document.querySelectorAll('input[data-location]:checked')].map(e=>config.locations[Number(e.dataset.location)]);
  if (!locations.length) {say('Select at least one location.');return;}
  let endpoint;
  try {endpoint=new URL($('endpoint').value);if(endpoint.protocol!=='https:')throw new Error();}
  catch {say('Enter an HTTPS map-service endpoint.');return;}
  stopped=false;leaseError='';$('start').disabled=true;$('stop').disabled=false;logLines.length=0;$('log').textContent='';$('summary').textContent='';
  let saved=0, skipped=0;
  try {
    await acquire();check();
    const plan=await (await local(url('/roads/plan'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({locations,rangeKm:config.rangeKm})})).json();
    const tiles=plan.tiles;skipped=tiles.filter(t=>t.cached).length;
    const pending=tiles.filter(t=>!t.cached).map(t=>({...t,attempts:0,retryAt:0}));
    $('progress').max=Math.max(1,tiles.length);$('progress').value=skipped;
    const summary=()=>{$('summary').textContent=saved+' uploaded · '+skipped+' already cached · '+pending.length+' pending';$('progress').value=skipped+saved;};
    summary();
    while(pending.length) {
      check();
      const index=pending.findIndex(t=>t.retryAt<=Date.now());
      if(index<0||Date.now()<rateLimitUntil){
        const wait=Math.max(1,Math.ceil((Math.max(rateLimitUntil,Math.min(...pending.map(t=>t.retryAt)))-Date.now())/1000));
        say(Date.now()<rateLimitUntil?'Map service rate limited requests. Retrying in '+wait+'s…':pending.length+' map sections pending. Retrying in '+wait+'s…');
        await sleep(500);continue;
      }
      const tile=pending[index];++tile.attempts;
      try {
        const doc=await download(tile,endpoint.href);check();say('Preparing '+tile.roadClass+'…\n'+tile.path);
        const binary=await RoadTiles.process(doc,tile);check();say('Saving to SD card…\n'+tile.path);
        const data=new FormData();data.append('tile',new Blob([binary]),'tile.bin');
        uploading=true;
        try {
          for(let attempt=0;;++attempt) {
            const r=await fetch(url('/roads/tile')+'&id='+tile.id,{method:'POST',body:data,signal:AbortSignal.timeout(90000)});
            if(r.ok)break;
            const message=(await r.text()).slice(0,200);
            if(r.status!==503||attempt>=2)throw new Error(message);
            check();say('SD card busy; retrying upload…');await sleep(1500);
          }
        }finally{uploading=false;}
        ++saved;pending.splice(index,1);log('Saved '+tile.path+' ('+binary.byteLength+' bytes)');
        // A completed background cell also covers its smaller starter cells.
        // Don't keep retrying a failed starter after its coverage is available.
        if(tile.span===4)for(let i=pending.length-1;i>=0;--i){
          const t=pending[i];
          if(t.span===1&&t.roadClass===tile.roadClass&&Math.floor(t.lat/4)===Math.floor(tile.lat/4)&&Math.floor(t.lon/4)===Math.floor(tile.lon/4)){
            pending.splice(i,1);++skipped;log('Covered '+t.path+' by '+tile.path);
          }
        }
      } catch(e) {
        check();
        const delay=Math.max(Math.min(300000,30000*2**Math.min(tile.attempts-1,4)),Math.ceil(((e.retryAt||0)-Date.now())/1000)*1000);
        tile.retryAt=Date.now()+delay;
        pending.splice(index,1);pending.push(tile);
        log('Retry pending '+tile.path+' (attempt '+tile.attempts+'): '+e.message+'; retry in '+delay/1000+'s');
      }
      summary();
    }
    say('Maps ready. You can close this page.');
  } catch(e) {say(e.message);}
  finally {
    clearInterval(heartbeat);
    if(token)try{await local(url('/roads/release'),{method:'POST'});}catch(e){log('Device downloads will resume automatically after the session expires.');}
    token='';$('start').disabled=false;$('stop').disabled=true;
  }
};
(async()=>{
  try {
    config=await (await local('/roads/locations.json')).json();
    $('locations').textContent='';
    config.locations.forEach((location,i)=>{
      const row=document.createElement('label');row.className='location';
      const box=document.createElement('input');box.type='checkbox';box.checked=i===0;box.dataset.location=i;
      const text=document.createElement('span');text.textContent=location.name;
      const coords=document.createElement('small');coords.textContent=location.lat.toFixed(4)+', '+location.lon.toFixed(4);text.append(coords);
      row.append(box,text);$('locations').append(row);
    });
    $('range').textContent='Coverage uses the Orb’s current radar range: '+config.rangeKm+' km.';
    $('endpoint').value=config.endpoint;
    $('start').disabled=!config.sdMounted||!config.locations.length;
    say(!config.sdMounted?'Insert an SD card, then reload this page.':config.locations.length?'Select locations to preload.':'Set a location on the Orb first.');
  }catch(e){say('Could not read locations: '+e.message);}
})();
</script></body></html>)HTML";
