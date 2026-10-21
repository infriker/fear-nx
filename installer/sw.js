// Adapted from nfsmw-nx-installer by StevensND (GPL-3.0),
// https://github.com/StevensND/nfsmw-nx-installer/blob/main/sw.js
// (cache names and the list of page files changed).
// Serves the zip that the page generates as a regular browser download, and keeps a copy of the page, its tools and
// the program of each edition the user has made a package for, so a package can still be made without a connection.
// The generator sends the zip through a MessagePort, one chunk per "pull", so memory use stays bounded however big
// the zip is.

const PAGE_CACHE = 'fear-nx-page';
const PROGRAM_CACHE = 'fear-nx-programs';
// Everything the page needs to make a package except the program, which is
// kept once the player has chosen a supported game.
const PAGE_FILES = [
  './',
  'index.html',
  'app.js',
  'worker.js',
  'style.css',
  'icon.jpg',
  'lib/i18n.js',
  'lib/installer.js',
  'lib/iso.js',
  'lib/zip.js',
  'release/manifest.json',
  'release/fear-nx.toml',
  'release/LICENSES.txt',
];
// How long a page file waits for the network before the kept copy is used (the network still refreshes it).
const NETWORK_WAIT = 4000;

const jobs = new Map();
// Programs being downloaded into the cache, by address, so two requests for the same one share the download.
const downloading = new Map();

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) =>
  event.waitUntil(
    (async () => {
      for (const name of await caches.keys()) {
        if (name.startsWith('fear-nx-') && name !== PAGE_CACHE && name !== PROGRAM_CACHE) {
          await caches.delete(name);
        }
      }
      await self.clients.claim();
    })(),
  ),
);

self.addEventListener('message', (event) => {
  const data = event.data || {};
  if (data.type === 'download') {
    const [port, ack] = event.ports;
    jobs.set(data.id, { port, size: data.size, filename: data.filename });
    ack.postMessage('registered');
  } else if (data.type === 'claim') {
    event.waitUntil(self.clients.claim());
  } else if (data.type === 'keep-page') {
    event.waitUntil(keepPage());
  } else if (data.type === 'keep-program' && typeof data.url === 'string') {
    event.waitUntil(keepProgram(data.url).catch(() => {}));
  }
  // "ping" messages only keep this worker alive while a download is running.
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  const url = new URL(request.url);
  const match = /\/download\/([\w-]+)\/[^/]+$/.exec(url.pathname);
  if (match) {
    const job = jobs.get(match[1]);
    if (job) {
      jobs.delete(match[1]);
      event.respondWith(serveDownload(job));
    }
    return;
  }
  if (request.method !== 'GET' || !url.protocol.startsWith('http')) {
    return;
  }
  if (url.searchParams.has('sha256')) {
    event.respondWith(fromPrograms(request));
  } else if (url.origin === self.location.origin) {
    const answer = fromPage(request);
    event.respondWith(answer.then((a) => a.response));
    event.waitUntil(answer.then((a) => a.stored));
  }
});

function serveDownload(job) {
  const { port } = job;
  const stream = new ReadableStream(
    {
      start(controller) {
        port.onmessage = ({ data }) => {
          if (data.type === 'chunk') {
            controller.enqueue(new Uint8Array(data.buffer));
          } else if (data.type === 'end') {
            controller.close();
          } else if (data.type === 'error') {
            controller.error(new Error(data.message));
          }
        };
      },
      pull() {
        port.postMessage({ type: 'pull' });
      },
      cancel() {
        port.postMessage({ type: 'cancel' });
      },
    },
    { highWaterMark: 4 },
  );
  const headers = {
    'Content-Type': 'application/zip',
    'Content-Disposition': `attachment; filename="${job.filename}"`,
    'Content-Length': String(job.size),
  };
  return new Response(stream, { headers });
}

function withoutQuery(address) {
  const url = new URL(address);
  url.search = '';
  url.hash = '';
  return url.href;
}

function wait(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// Page files: the network when it answers, the kept copy when it fails, has a server error or keeps the page
// waiting. Whatever the network brings replaces the kept copy, unless it is the same version.
async function fromPage(request) {
  const cache = await caches.open(PAGE_CACHE);
  const key = withoutQuery(request.url);
  const kept =
    (await cache.match(key)) ||
    (request.mode === 'navigate' ? await cache.match(new URL('./', self.registration.scope).href) : undefined);
  const network = fetch(request);
  const stored = network.then(
    (fresh) => (fresh.ok && fresh.type === 'basic' ? store(cache, key, fresh.clone(), kept) : undefined),
    () => undefined,
  );
  if (!kept) {
    return { response: network, stored };
  }
  const response = Promise.race([
    network.then((fresh) => (fresh.status >= 500 ? kept : fresh), () => kept),
    wait(NETWORK_WAIT).then(() => kept),
  ]);
  return { response, stored };
}

async function store(cache, key, fresh, kept) {
  if (kept && sameVersion(kept, fresh)) {
    await fresh.body?.cancel();
    return;
  }
  try {
    await cache.put(key, fresh);
  } catch {
    // no room left: the page keeps working from the network
  }
}

function sameVersion(kept, fresh) {
  for (const name of ['etag', 'last-modified']) {
    const value = fresh.headers.get(name);
    if (value) {
      return value === kept.headers.get(name) && fresh.headers.get('content-length') === kept.headers.get('content-length');
    }
  }
  return false;
}

// Programs: their address names their SHA-256, so a kept copy is always the right one and is used first.
async function fromPrograms(request) {
  const cache = await caches.open(PROGRAM_CACHE);
  let kept = await cache.match(request.url);
  if (!kept) {
    try {
      await keepProgram(request.url);
      kept = await cache.match(request.url);
    } catch {
      // it could not be kept (no room): straight from the network
    }
  }
  return kept || fetch(request);
}

// Downloads a program into the cache, where it replaces the older versions of the same file.
function keepProgram(url) {
  if (!downloading.has(url)) {
    const job = (async () => {
      const cache = await caches.open(PROGRAM_CACHE);
      if (await cache.match(url)) {
        return;
      }
      const response = await fetch(url);
      if (!response.ok) {
        throw new Error(`${url}: HTTP ${response.status}`);
      }
      await cache.put(url, response);
      // the older versions go only once the new one is complete, so an interrupted download leaves the old one
      const file = withoutQuery(url);
      for (const old of await cache.keys()) {
        if (old.url !== url && withoutQuery(old.url) === file) {
          await cache.delete(old);
        }
      }
    })().finally(() => downloading.delete(url));
    downloading.set(url, job);
  }
  return downloading.get(url);
}

// Fills the cache with the page files it does not have yet (the first visit), so the page also works offline.
async function keepPage() {
  const cache = await caches.open(PAGE_CACHE);
  await Promise.all(
    PAGE_FILES.map(async (file) => {
      const url = new URL(file, self.registration.scope).href;
      if (await cache.match(url)) {
        return;
      }
      try {
        const response = await fetch(url);
        if (response.ok) {
          await cache.put(url, response);
        }
      } catch {
        // offline or no room: it is kept the next time it is used
      }
    }),
  );
}
