import test from 'node:test';
import assert from 'node:assert/strict';
import {identifyLayout, validateRelease, validateCache, flashPlan, PARTS, resetToFirmware} from '../../web/companion/layout.mjs';
const table = entries => {
  const bytes = new Uint8Array(4096).fill(255), view = new DataView(bytes.buffer);
  entries.forEach(([name, kind, subtype, offset, size], index) => {
    const p = index * 32; view.setUint16(p, 0x50AA, true); bytes[p + 2] = kind; bytes[p + 3] = subtype;
    view.setUint32(p + 4, offset, true); view.setUint32(p + 8, size, true);
    bytes.fill(0, p + 12, p + 32); bytes.set(new TextEncoder().encode(name), p + 12);
  }); return bytes;
};
const original = table([['nvs',1,2,0x9000,0x5000],['app0',0,0,0x10000,0x640000],
  ['themeart',1,64,0x650000,0x9A0000],['coredump',1,3,0xFF0000,0x10000]]);
const companion = table([['nvs',1,2,0x9000,0x5000],['otadata',1,0,0xE000,0x2000],
  ['ota_0',0,16,0x10000,0x640000],['themeart',1,64,0x650000,0x480000],
  ['ota_1',0,17,0xAD0000,0x400000],['ps_nvs',1,2,0xED0000,0x80000],
  ['sounds',1,131,0xF50000,0xA0000],['coredump',1,3,0xFF0000,0x10000]]);
const files = Object.fromEntries(Object.entries(PARTS).map(([name, [, size]]) => [name, new Uint8Array(size)]));
test('identifies exact standalone, companion, blank, and foreign layouts', () => {
  assert.equal(identifyLayout(original), 'orb'); assert.equal(identifyLayout(companion), 'companion');
  assert.equal(identifyLayout(new Uint8Array(4096).fill(255)), 'blank');
  const wrong = companion.slice(); wrong[8] ^= 1; assert.equal(identifyLayout(wrong), 'unknown');
});
test('routine updates touch only the two firmware slots', () => {
  const plan = flashPlan('companion', files);
  assert.deepEqual(plan.map(p => p.address), [0x10000,0xAD0000]);
  assert.deepEqual(plan.map(p => p.data.length), [0x640000,0x400000]);
});
test('migration writes never overlap Orb NVS or theme cache, even with maximum images', () => {
  const plan = flashPlan('orb', files);
  for (const part of plan) for (const [start,end] of [[0x9000,0xE000],[0x650000,0xAD0000]]) {
    assert.ok(part.address + part.data.length <= start || part.address >= end);
  }
  assert.equal(plan.at(-1).address, 0x8000);
  assert.throws(() => flashPlan('unknown',files));
});
test('release metadata cannot change destinations, add files, or exceed capacity', () => {
  const valid = {schema:1,layout:'orb-printsphere-v1',chip:'ESP32-S3',flashSize:0x1000000,
    parts:Object.entries(PARTS).map(([path,[offset,size]])=>({path,offset,size,sha256:'a'.repeat(64)}))};
  validateRelease(valid);
  for (const edit of [m=>m.parts[0].offset=0x9000,m=>m.parts[0].path='../settings.bin',
    m=>m.parts[2].size++,m=>m.parts.push(m.parts[0]),m=>m.parts[0].sha256='bad']) {
    const bad = structuredClone(valid); edit(bad); assert.throws(()=>validateRelease(bad));
  }
});
test('migration rejects overfull caches and entries crossing the retained boundary', () => {
  const bytes = new Uint8Array(8192), v = new DataView(bytes.buffer);
  v.setUint32(0,0x4F524254,true); v.setUint32(4,6,true); validateCache(bytes);
  v.setUint32(12,0x480000,true); assert.throws(()=>validateCache(bytes)); v.setUint32(12,0,true);
  v.setUint32(8,1,true); v.setUint32(16+44,0x47F000,true); v.setUint32(16+48,8192,true);
  assert.throws(()=>validateCache(bytes));
});


test('opt-in rebuild clears only the cache index and commits migration layout last', () => {
  for (const layout of ['orb', 'companion']) {
    const plan = flashPlan(layout, files, true);
    const reset = plan.filter(p => p.address === 0x650000);
    assert.equal(reset.length, 1);
    assert.equal(reset[0].data.length, 8192);
    assert.ok(reset[0].data.every(b => b === 255));
    for (const p of plan) assert.ok(p.address + p.data.length <= 0x9000 || p.address >= 0xE000);
    if (layout === 'orb') assert.equal(plan.at(-1).address, 0x8000);
    else assert.deepEqual(plan.map(p => p.address), [0x10000, 0xAD0000, 0x650000]);
  }
  assert.deepEqual(flashPlan('blank', files, true), flashPlan('blank', files));
  validateCache(new Uint8Array(8192).fill(255));
  assert.throws(() => flashPlan('unknown', files, true));
});


test('installer migration needs no backup and gates an oversized cache on explicit consent', async () => {
  const {readFile} = await import('node:fs/promises');
  const vm = await import('node:vm');
  const source = (await readFile(new URL('../../web/companion/installer.mjs', import.meta.url), 'utf8'))
    .replace(/^import .*?;\s*/, '')
    .replace("await import('https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/bundle.js')", 'fakeTools');
  for (const overfull of [false, true]) {
    const nodes = new Map(), reads = [], writes = [];
    const element = id => {
      if (!nodes.has(id)) nodes.set(id, {value: '0', checked: false, textContent: '', hidden: false,
        classList: {toggle() {}}, listeners: {}, replaceChildren() {},
        closest(selector) {
          assert.equal(id, 'log');
          assert.equal(selector, 'details');
          return element('connection-details');
        },
        addEventListener(name, fn) { this.listeners[name] = fn; }});
      return nodes.get(id);
    };
    const cache = new Uint8Array(8192), view = new DataView(cache.buffer);
    view.setUint32(0, 0x4F524254, true); view.setUint32(4, 6, true);
    if (overfull) view.setUint32(12, 0x480000, true);
    class Loader {
      chip = {CHIP_NAME: 'ESP32-S3'};
      async main() {} async detectFlashSize() { return '16MB'; }
      async readFlash(address, size) {
        reads.push([address, size]);
        return address === 0x8000 ? original : address === 0x650000 ? cache : new Uint8Array(size);
      }
      async flashMd5sum() { return 'verified'; }
      async writeFlash(options) { writes.push(options); }
      async after() {}
    }
    class Transport { async disconnect() {} async setDTR() {} async setRTS() {} }
    const sandbox = {FLASH_SIZE: 0x1000000, require: (ok,msg) => { if (!ok) throw Error(msg); },
      identifyLayout, validateCache, flashPlan, resetToFirmware, validateRelease() {}, validateImage() {},
      document: {getElementById: element, createElement: () => ({})},
      navigator: {serial: {requestPort: async () => ({})}},
      window: {addEventListener() {}}, location: {href: 'https://example.com/', origin: 'https://example.com'},
      ResizeObserver: class { observe() {} },
      SparkMD5: {ArrayBuffer: {hash: () => 'verified'}},
      fakeTools: {ESPLoader: Loader, Transport}, URL, Uint8Array, crypto: {subtle: {digest: async () => new Uint8Array(32)}},
      fetch: async url => String(url).includes('release-index')
        ? {ok:true, json:async () => [{tag:'test',manifest:'manifest.json'}]}
        : String(url).includes('manifest.json')
          ? {ok:true,json:async () => ({parts:Object.keys(PARTS).map(path => ({path,size:256,sha256:'0'.repeat(64)}))})}
          : {ok:true,arrayBuffer:async () => new Uint8Array(256).buffer}};
    await vm.runInNewContext('(async () => {' + source + '})()', sandbox);
    await element('connect').listeners.click();
    assert.equal(element('ready').hidden, false);
    assert.equal(element('install').disabled, overfull);
    assert.equal(element('rebuild-cache').checked, false);
    assert.ok(reads.every(([,size]) => size <= 8192)); // no full-flash backup
    if (overfull) {
      element('rebuild-cache').checked = true;
      element('rebuild-cache').listeners.change();
      assert.equal(element('install').disabled, false);
      element('rebuild-cache').checked = false;
      element('rebuild-cache').listeners.change();
      assert.equal(element('install').disabled, true);
      element('rebuild-cache').checked = true;
      element('rebuild-cache').listeners.change();
    }
    await element('install').listeners.click();
    assert.equal(writes.length, 1);
    assert.equal(writes[0].eraseAll, false);
    assert.equal(writes[0].fileArray.some(p => p.address === 0x650000), overfull);
    assert.equal(reads.filter(([address]) => address === 0x9000).length, 2);
    assert.match(element('status').textContent, /Installation complete and verified/);
  }
});


test('firmware reset deasserts boot strap, pulses reset, and waits before closing', async () => {
  const calls = [];
  await resetToFirmware({setDTR: async v => calls.push(['DTR', v]),
    setRTS: async v => calls.push(['RTS', v])}, async ms => calls.push(['wait', ms]));
  assert.deepEqual(calls, [['DTR', false], ['RTS', true], ['wait', 200], ['RTS', false], ['wait', 200]]);
});

test('reset failures propagate and still attempt to release reset', async () => {
  const calls = [];
  await assert.rejects(resetToFirmware({setDTR: async () => {}, setRTS: async v => {
    calls.push(v); if (v) throw new Error('USB reset failed');
  }}, async () => {}), /USB reset failed/);
  assert.deepEqual(calls, [true, false]);
});
