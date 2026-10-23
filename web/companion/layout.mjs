// Fixed installation contract. Release metadata cannot redirect writes elsewhere.
export const FLASH_SIZE = 0x1000000;
export const PARTS = Object.freeze({
  'orb-bootloader.bin': [0, 0x8000],
  'orb-partitions.bin': [0x8000, 0x1000],
  'Orb-companion.bin': [0x10000, 0x640000],
  'PrintSphere-companion.bin': [0xAD0000, 0x400000],
});
const EXPECTED = {
  nvs: [1, 2, 0x9000, 0x5000], otadata: [1, 0, 0xE000, 0x2000],
  ota_0: [0, 16, 0x10000, 0x640000], themeart: [1, 64, 0x650000, 0x480000],
  ota_1: [0, 17, 0xAD0000, 0x400000], ps_nvs: [1, 2, 0xED0000, 0x80000],
  sounds: [1, 131, 0xF50000, 0xA0000], coredump: [1, 3, 0xFF0000, 0x10000],
};
const ORIGINAL = {nvs: EXPECTED.nvs, app0: [0, 0, 0x10000, 0x640000],
  themeart: [1, 64, 0x650000, 0x9A0000], coredump: EXPECTED.coredump};
export function require(condition, message) { if (!condition) throw new Error(message); }
const view = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
const string = bytes => new TextDecoder().decode(bytes).split('\0')[0];
function partitions(bytes) {
  const table = {}, data = view(bytes);
  for (let p = 0; p + 32 <= bytes.length && data.getUint16(p, true) === 0x50AA; p += 32) {
    const name = string(bytes.slice(p + 12, p + 28));
    require(!Object.hasOwn(table, name) && data.getUint32(p + 28, true) === 0, 'Unrecognized partition flags.');
    table[name] = [bytes[p + 2], bytes[p + 3], data.getUint32(p + 4, true), data.getUint32(p + 8, true)];
  }
  return table;
}
function matches(actual, expected) {
  return Object.keys(actual).length === Object.keys(expected).length &&
    Object.entries(expected).every(([key, value]) => JSON.stringify(actual[key]) === JSON.stringify(value));
}
export function identifyLayout(bytes) {
  const table = partitions(bytes);
  if (matches(table, EXPECTED)) return 'companion';
  if (matches(table, ORIGINAL)) return 'orb';
  if (bytes.every(b => b === 255)) return 'blank';
  return 'unknown';
}
export function validateRelease(release) {
  require(release.schema === 1 && release.layout === 'orb-printsphere-v1' &&
    release.chip === 'ESP32-S3' && release.flashSize === FLASH_SIZE, 'This release uses an unsupported device layout.');
  require(Array.isArray(release.parts) && release.parts.length === 4, 'Incomplete firmware release.');
  const names = new Set();
  for (const part of release.parts) {
    const expected = PARTS[part.path];
    require(expected && !names.has(part.path) && part.offset === expected[0] &&
      Number.isInteger(part.size) && part.size > 0 && part.size <= expected[1] &&
      /^[a-f0-9]{64}$/.test(part.sha256), 'Invalid release image or address.');
    names.add(part.path);
  }
}
export function validateImage(name, bytes) {
  require(bytes.length > 0 && bytes.length <= PARTS[name][1], 'Firmware image is too large.');
  if (name === 'orb-partitions.bin') {
    require(identifyLayout(bytes) === 'companion', 'Release partition table does not match this installer.');
    return;
  }
  require(bytes[0] === 0xE9 && bytes.length > 24 && view(bytes).getUint16(12, true) === 9,
    'Firmware is not an ESP32-S3 image.');
  if (name === 'orb-bootloader.bin') return;
  require(bytes.length >= 256 && view(bytes).getUint32(32, true) === 0xABCD5432, 'Missing firmware identity.');
  if (name === 'PrintSphere-companion.bin') {
    require(string(bytes.slice(80, 112)) === 'printsphere_idf' && /-orb(test)?$/.test(string(bytes.slice(48, 80))),
      'PrintSphere image is not adapted for Orb.');
  } else {
    require(new TextDecoder('latin1').decode(bytes).includes('Opening PrintSphere...'), 'Orb companion launcher is missing.');
  }
}
export function validateCache(bytes) {
  require(bytes.length >= 8192, 'Theme cache could not be read.');
  const data = view(bytes), magic = data.getUint32(0, true);
  if (magic === 0xFFFFFFFF) return;
  require(magic === 0x4F524254 && data.getUint32(4, true) === 6, 'Unrecognized Orb theme cache. Nothing was changed.');
  const count = data.getUint32(8, true), used = data.getUint32(12, true);
  require(count <= 127 && used + 8192 <= 0x480000,
    'Your cached themes need more space than this layout allows. Select Rebuild theme cache to continue.');
  for (let n = 0; n < count; n++) {
    const position = 16 + n * 64, offset = data.getUint32(position + 44, true), length = data.getUint32(position + 48, true);
    require(offset >= 8192 && offset + length <= 0x480000, 'A cached theme would be truncated. Nothing was changed.');
  }
}
export function flashPlan(layout, files, rebuildCache = false) {
  require(['companion', 'orb', 'blank'].includes(layout), 'Unsupported existing firmware. Nothing was changed.');
  const appNames = ['Orb-companion.bin', 'PrintSphere-companion.bin'];
  const cacheReset = {address: 0x650000, data: new Uint8Array(8192).fill(255), name: 'Rebuild theme cache'};
  if (layout === 'companion') {
    const plan = appNames.map(name => ({address: PARTS[name][0], data: files[name], name}));
    if (rebuildCache) plan.push(cacheReset);
    return plan;
  }
  // Separate writes leave Orb NVS and themeart untouched. No full-chip erase.
  const names = ['orb-bootloader.bin', ...appNames];
  const result = names.map(name => ({address: PARTS[name][0], data: files[name], name}));
  result.push({address: 0xE000, data: new Uint8Array(8192).fill(255), name: 'Start in Orb'});
  result.push({address: 0xED0000, data: new Uint8Array(0x120000).fill(255), name: 'Initialize PrintSphere settings'});
  if (rebuildCache && layout === 'orb') result.push(cacheReset);
  // Commit the new layout last, after all images are present.
  result.push({address: 0x8000, data: files['orb-partitions.bin'], name: 'Activate companion layout'});
  return result;
}

// esptool-js 0.7.0 HardReset only releases RTS; it never asserts reset.
// Match esptool's native-USB timing and keep the boot strap deasserted.
export async function resetToFirmware(transport, wait = ms => new Promise(resolve => setTimeout(resolve, ms))) {
  await transport.setDTR(false);
  try {
    await transport.setRTS(true);
    await wait(200);
  } finally {
    await transport.setRTS(false);
  }
  await wait(200); // let the USB peripheral settle before closing the port
}
