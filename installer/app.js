// The page: choosing the game, identifying it, and running the worker that
// makes the package. Adapted from nfsmw-nx-installer by StevensND (GPL-3.0).
import { listIsoFiles } from './lib/iso.js';
import { TITLE_ID, identifyGame, programPath, rootAtExecutable } from './lib/installer.js';
import { formatSize, t } from './lib/i18n.js';

const chosen = document.getElementById('chosen');
const create = document.getElementById('create');
const update = document.getElementById('update');
const bar = document.getElementById('bar');
const logBox = document.getElementById('log');
const detected = document.getElementById('detected');
const detectedTitle = document.getElementById('detectedTitle');
const detectedState = document.getElementById('detectedState');
const fingerprint = document.getElementById('fingerprint');
const manifest = fetch('./release/manifest.json').then((r) => r.json());
let source = null;
let check = 0;
// What the page shows about the chosen game, kept as data so it can be drawn
// as detection progresses.
let chosenView = null;
let detectedView = null;
let downloadFrame = null;

function log(text, error = false) {
  const line = document.createElement('div');
  line.textContent = text;
  if (error) {
    line.className = 'error';
  }
  logBox.appendChild(line);
  logBox.scrollTop = logBox.scrollHeight;
}

function renderChosen() {
  if (!chosenView) {
    chosen.textContent = t('nothingChosen');
  } else if (chosenView.count === undefined) {
    chosen.textContent = t('chosenFile', { name: chosenView.name, size: formatSize(chosenView.size) });
  } else {
    chosen.textContent = t(chosenView.count === 1 ? 'chosenFolderOne' : 'chosenFolder',
      { name: chosenView.name, count: chosenView.count, size: formatSize(chosenView.size) });
  }
}

function showDetected(state, title, text, hash = '') {
  detected.hidden = false;
  detected.className = `detected ${state}`;
  detectedTitle.textContent = title;
  detectedState.textContent = text;
  detectedState.className = `state ${state}`;
  fingerprint.textContent = hash;
  fingerprint.hidden = !hash;
}

function renderDetected() {
  const v = detectedView;
  if (!v) {
    detected.hidden = true;
    return;
  }
  switch (v.kind) {
    case 'checking':
      showDetected('', t('checking'), '');
      break;
    case 'supported':
      showDetected('good', t('gameName'), t('supported'));
      break;
    case 'executableOnly':
      showDetected('bad', t('gameName'), t('executableOnly'));
      break;
    case 'wrongRelease':
      showDetected('bad', t('gameName'), t('wrongRelease'), v.hash);
      break;
    case 'notGame':
      showDetected('bad', 'default.xex', t('notGame'));
      break;
    case 'notComplete':
      showDetected('bad', t('notComplete'), v.code ? t(v.code) : v.message);
      break;
    case 'notIso':
      showDetected('bad', t('notIso'), v.message);
      break;
  }
}

for (const el of document.querySelectorAll('[data-i18n]')) {
  el.textContent = t(el.dataset.i18n);
}
// Only the project's own static strings contain markup.
for (const el of document.querySelectorAll('[data-i18n-html]')) {
  el.innerHTML = t(el.dataset.i18nHtml);
}
renderChosen();

function setButtons(full, onlyUpdate = full) {
  create.disabled = !full;
  update.disabled = !onlyUpdate;
}

// Identifies the game as soon as it is chosen.
async function identify(files) {
  const id = ++check;
  setButtons(false);
  detectedView = { kind: 'checking' };
  renderDetected();
  try {
    const game = await identifyGame(rootAtExecutable(files), await manifest);
    if (id !== check) {
      return;
    }
    if (game.build) {
      detectedView = { kind: game.complete ? 'supported' : 'executableOnly' };
      setButtons(game.complete, true);
      // the program is kept by the browser now, so the package can be made
      // even if the connection drops before it is
      const url = new URL(programPath(game.build), location.href).href;
      helper.then((controller) => controller.postMessage({ type: 'keep-program', url }), () => {});
    } else if (game.titleId === TITLE_ID) {
      detectedView = { kind: 'wrongRelease', hash: game.xexHash };
    } else {
      detectedView = { kind: 'notGame' };
    }
  } catch (error) {
    if (id !== check) {
      return;
    }
    detectedView = { kind: 'notComplete', code: error.code, message: error.message };
  }
  renderDetected();
}

document.getElementById('iso').addEventListener('change', async (event) => {
  const file = event.target.files[0];
  if (!file) {
    return;
  }
  source = { kind: 'iso', file };
  chosenView = { name: file.name, size: file.size };
  renderChosen();
  let files;
  try {
    files = await listIsoFiles(file);
  } catch (error) {
    ++check;
    setButtons(false);
    detectedView = { kind: 'notIso', message: error.message };
    renderDetected();
    return;
  }
  await identify(files);
});

document.getElementById('folder').addEventListener('change', async (event) => {
  const list = [...event.target.files];
  if (!list.length) {
    return;
  }
  const top = list[0].webkitRelativePath.split('/')[0] + '/';
  const entries = list.map((file) => ({
    path: file.webkitRelativePath.startsWith(top) ? file.webkitRelativePath.slice(top.length) : file.webkitRelativePath,
    file,
  }));
  source = { kind: 'folder', entries };
  chosenView = { name: top.slice(0, -1), count: list.length, size: list.reduce((sum, f) => sum + f.size, 0) };
  renderChosen();
  await identify(entries.map(({ path, file }) => ({
    path,
    size: file.size,
    read: async (offset, length) => new Uint8Array(await file.slice(offset, offset + length).arrayBuffer()),
  })));
});

// The zip is served by sw.js as a normal browser download, streamed while it
// is being made, so a package of several GB never has to fit in memory.
async function downloadHelper() {
  if (!('serviceWorker' in navigator)) {
    throw new Error('noStreaming');
  }
  await navigator.serviceWorker.register('./sw.js');
  const registration = await navigator.serviceWorker.ready;
  if (!navigator.serviceWorker.controller) {
    const claimed = new Promise((resolve) =>
      navigator.serviceWorker.addEventListener('controllerchange', resolve, { once: true }));
    // a forced reload opens the page without its service worker, which then has to take it back
    registration.active.postMessage({ type: 'claim' });
    await claimed;
  }
  navigator.serviceWorker.controller.postMessage({ type: 'keep-page' });
  return navigator.serviceWorker.controller;
}
const helper = downloadHelper();
helper.catch(() => {});

async function makePackage(onlyUpdate) {
  if (!source) {
    return;
  }
  let controller;
  try {
    controller = await helper;
  } catch (error) {
    log(t('error', { message: error.message === 'noStreaming' ? t('noStreaming') : error.message }), true);
    return;
  }
  const filename = onlyUpdate ? 'fear-nx-update.zip' : 'fear-nx.zip';
  const wasFull = !create.disabled;
  setButtons(false);
  bar.hidden = false;
  bar.value = 0;
  logBox.textContent = '';
  const started = Date.now();
  const channel = new MessageChannel();
  const keepAlive = setInterval(() => controller.postMessage({ type: 'ping' }), 10000);
  const worker = new Worker('./worker.js', { type: 'module' });
  const finish = () => {
    clearInterval(keepAlive);
    worker.terminate();
    setButtons(wasFull, true);
  };
  worker.onmessage = async ({ data }) => {
    if (data.type === 'log') {
      log(data.text);
    } else if (data.type === 'progress') {
      bar.value = data.fraction;
    } else if (data.type === 'size') {
      const id = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
      const ack = new MessageChannel();
      const registered = new Promise((resolve) => (ack.port1.onmessage = resolve));
      controller.postMessage({ type: 'download', id, filename, size: data.size }, [channel.port2, ack.port2]);
      await registered;
      // a hidden frame opens the download: its request is a navigation, which
      // always reaches the service worker (Chrome sends the request of an
      // <a download> link straight to the network, where the file does not exist)
      downloadFrame?.remove();
      downloadFrame = document.createElement('iframe');
      downloadFrame.hidden = true;
      downloadFrame.src = `./download/${id}/${filename}`;
      document.body.appendChild(downloadFrame);
      log(t('downloadStarted', { size: formatSize(data.size) }));
    } else if (data.type === 'done') {
      bar.value = 1;
      log(t('finished', { seconds: Math.round((Date.now() - started) / 1000) }));
      finish();
    } else if (data.type === 'error') {
      log(t('error', { message: data.message }), true);
      finish();
    }
  };
  worker.onerror = (event) => {
    log(t('error', { message: event.message }), true);
    finish();
  };
  worker.postMessage({ ...source, update: onlyUpdate, port: channel.port1 }, [channel.port1]);
}

create.addEventListener('click', () => makePackage(false));
update.addEventListener('click', () => makePackage(true));
