import {FLASH_SIZE, require, identifyLayout, validateRelease, validateImage, validateCache, flashPlan, resetToFirmware} from './layout.mjs';
const $ = id => document.getElementById(id);
let releases = [], loader, transport, layout, settingsHash, cacheError = '', busy = false;
const connectionLog = $('log');
const connectionDetails = connectionLog.closest('details');
let logScrollFrame;
function followConnectionLog() {
  if (!connectionDetails.open || logScrollFrame !== undefined) return;
  // Follow after layout, including when opening the details or wrapping lines.
  logScrollFrame = requestAnimationFrame(() => {
    logScrollFrame = undefined;
    if (connectionDetails.open) connectionLog.scrollTop = connectionLog.scrollHeight;
  });
}
connectionDetails.addEventListener('toggle', followConnectionLog);
new ResizeObserver(followConnectionLog).observe(connectionLog);
const log = message => {
  connectionLog.textContent = (connectionLog.textContent + message + '\n').slice(-14000);
  followConnectionLog();
};
function status(message, error = false) { $('status').textContent = message; $('status').classList.toggle('error', error); }
function progress(value) { $('progress').hidden = false; $('progress').value = value; }
function lock(value) {
  busy = value;
  $('connect').disabled = value || !!loader || !releases.length || !('serial' in navigator);
  $('release').disabled = value || !!loader || !releases.length;
  $('disconnect').disabled = value;
  $('install').disabled = value || !layout || (!!cacheError && !$('rebuild-cache').checked);
  $('rebuild-cache').disabled = value;
}
const md5 = bytes => SparkMD5.ArrayBuffer.hash(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
async function readVerified(address, size) {
  const result = await loader.readFlash(address, size);
  require(result.length === size, 'The device returned an incomplete read.');
  require(md5(result) === await loader.flashMd5sum(address, size), 'Device read verification failed. Nothing was written.');
  return result;
}
async function downloadRelease() {
  const entry = releases[Number($('release').value)];
  const url = new URL(entry.manifest, location.href);
  require(url.origin === location.origin, 'Release manifest must be served by this installer.');
  const response = await fetch(url, {cache: 'no-store'});
  require(response.ok, 'Could not load the release manifest.');
  const manifest = await response.json();
  validateRelease(manifest);
  const files = {};
  for (const part of manifest.parts) {
    status('Downloading and checking ' + part.path + '…');
    const response = await fetch(new URL(part.path, url));
    require(response.ok, 'Could not download ' + part.path);
    const bytes = new Uint8Array(await response.arrayBuffer());
    const digest = [...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))].map(b => b.toString(16).padStart(2, '0')).join('');
    require(bytes.length === part.size && digest === part.sha256, 'Release checksum mismatch: ' + part.path);
    validateImage(part.path, bytes);
    files[part.path] = bytes;
  }
  return files;
}
async function disconnect(reset = true) {
  let restarted = false;
  if (transport && reset) {
    try {
      log('Restarting firmware with an explicit DTR/RTS reset pulse...');
      await resetToFirmware(transport);
      restarted = true;
    } catch (error) { log('Automatic restart failed: ' + error.message); }
  }
  if (transport) { try { await transport.disconnect(); } catch (error) { log(error.message); } }
  loader = transport = layout = settingsHash = undefined;
  cacheError = '';
  $('rebuild-cache').checked = false;
  $('ready').hidden = $('disconnect').hidden = true;
  lock(false);
  return restarted;
}
$('connect').addEventListener('click', async () => {
  lock(true);
  try {
    // Request the port directly from the click, before asynchronous module loading.
    const port = await navigator.serial.requestPort();
    status('Connecting and checking your device…');
    require(typeof SparkMD5 !== 'undefined', 'Verification library could not load. Reload this page.');
    const {ESPLoader, Transport} = await import('https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/bundle.js');
    transport = new Transport(port);
    loader = new ESPLoader({transport, baudrate: 460800, terminal: {clean() {}, write: log, writeLine: log}});
    await loader.main();
    require(loader.chip.CHIP_NAME === 'ESP32-S3' && await loader.detectFlashSize() === '16MB',
      'This installer requires the 16 MB ESP32-S3 AMOLED 1.75 device.');
    layout = identifyLayout(await readVerified(0x8000, 4096));
    require(layout !== 'unknown', 'This device has another firmware layout. Use its recovery tools before installing Orb Companion. Nothing was changed.');
    cacheError = '';
    $('rebuild-cache').checked = false;
    $('cache-option').hidden = layout === 'blank';
    if (layout === 'orb') {
      const cache = await readVerified(0x650000, 8192);
      try { validateCache(cache); }
      catch (error) { cacheError = error.message; }
    }
    if (layout === 'blank') {
      // An erased partition table alone does not establish that the device is empty.
      // Check in bounded chunks without retaining or downloading a firmware backup.
      status('Checking that the device is empty...');
      for (let address = 0; address < FLASH_SIZE; address += 0x40000) {
        require((await readVerified(address, 0x40000)).every(b => b === 255),
          'Unrecognized data on this device. Nothing was changed.');
        progress(100 * (address + 0x40000) / FLASH_SIZE);
      }
    }
    $('install').textContent = layout === 'companion' ? 'Update both firmwares' : 'Install both firmwares';
    updatePlan();
    $('ready').hidden = $('disconnect').hidden = false;
    $('progress').hidden = true;
    status(cacheError ? cacheError : 'Device checked. Ready when you are.', !!cacheError);
  } catch (error) {
    await disconnect();
    status(error.name === 'NotFoundError' ? 'No device selected. Connect when you are ready.' : error.message, true);
  } finally { lock(false); }
});
function updatePlan() {
  const rebuild = $('rebuild-cache').checked;
  $('plan').textContent = layout === 'blank'
    ? 'Ready for a first installation of Orb and PrintSphere.'
    : (layout === 'companion' ? 'Ready to update both firmwares. ' : 'Ready to add PrintSphere. ') +
      (rebuild ? 'Your settings and SD themes will be kept. Cached artwork will be rebuilt on the next boot.'
               : 'Your settings, SD themes and cached artwork will be kept.');
}
$('rebuild-cache').addEventListener('change', () => {
  updatePlan(); lock(false);
  status(cacheError && !$('rebuild-cache').checked ? cacheError : 'Device checked. Ready when you are.',
    !!cacheError && !$('rebuild-cache').checked);
});
$('disconnect').addEventListener('click', async () => {
  lock(true);
  const restarted = await disconnect();
  status(restarted ? 'Disconnected. Restart requested.' : 'Disconnected. Unplug and reconnect USB to restart the device.', !restarted);
});
$('install').addEventListener('click', async () => {
  lock(true);
  let wrote = false;
  try {
    const files = await downloadRelease();
    require(identifyLayout(await readVerified(0x8000, 4096)) === layout, 'Device layout changed. Reconnect before installing.');
    const rebuild = $('rebuild-cache').checked;
    if (layout === 'orb' && !rebuild) validateCache(await readVerified(0x650000, 8192));
    if (layout !== 'blank') settingsHash = md5(await readVerified(0x9000, 0x5000));
    const plan = flashPlan(layout, files, rebuild);
    status('Installing both firmwares. Keep this page open and the cable connected…');
    wrote = true;
    await loader.writeFlash({fileArray: plan, flashSize: 'keep', flashMode: 'keep', flashFreq: 'keep',
      eraseAll: false, compress: true, calculateMD5Hash: md5,
      reportProgress: (file, done, total) => progress(100 * (file + done / total) / plan.length)});
    // Check private storage survived an in-place migration before rebooting.
    if (layout !== 'blank') {
      require(md5(await readVerified(0x9000, 0x5000)) === settingsHash, 'Orb settings verification failed.');
    }
    const restarted = await disconnect();
    progress(100);
    status(restarted ? 'Installation complete and verified. Restart requested.'
      : 'Installation complete and verified, but automatic restart failed. Unplug and reconnect USB to start the device.', !restarted);
  } catch (error) {
    // Do not boot a partially written image. Reconnect and retry.
    await disconnect(!wrote);
    status(error.message + (wrote ? ' Installation did not finish. Reconnect and retry.' : ' Nothing was written.'), true);
  } finally { lock(false); }
});
window.addEventListener('beforeunload', event => { if (busy) { event.preventDefault(); event.returnValue = ''; } });
try {
  const response = await fetch('release-index.json', {cache: 'no-store'});
  require(response.ok, 'Release list is not available yet. Publish a companion release first.');
  releases = await response.json();
  require(Array.isArray(releases) && releases.length, 'No companion releases have been published yet.');
  $('release').replaceChildren(...releases.map((release, index) => {
    const option = document.createElement('option'); option.value = index;
    option.textContent = release.tag + (index === 0 ? ' · Latest' : '') + (release.prerelease ? ' · Preview' : '');
    return option;
  }));
  status('Choose a release, then connect your device.');
} catch (error) { status(error.message, true); }
if (!('serial' in navigator)) {
  $('support').textContent = 'USB installation needs Chrome or Edge on a desktop computer. Open this page there to continue.';
}
lock(false);
