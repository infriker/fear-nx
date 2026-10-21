// Copied unchanged from nfsmw-nx-installer by StevensND (GPL-3.0),
// https://github.com/StevensND/nfsmw-nx-installer/blob/main/lib/iso.js
// Reads the XDVDFS file system of an Xbox 360 disc image (full Redump image or trimmed xiso) through a
// Blob/File, without loading the image into memory.

const SECTOR = 2048;
const MAGIC = 'MICROSOFT*XBOX*MEDIA';
const KNOWN_BASES = [0x00000000, 0x0fd90000, 0x02080000, 0x18300000];

async function readBytes(blob, offset, length) {
  return new Uint8Array(await blob.slice(offset, offset + length).arrayBuffer());
}

function text(bytes, start, length) {
  let s = '';
  for (let i = 0; i < length; i++) {
    s += String.fromCharCode(bytes[start + i]);
  }
  return s;
}

async function hasMagic(blob, base) {
  if (base + 32 * SECTOR + MAGIC.length > blob.size) {
    return false;
  }
  return text(await readBytes(blob, base + 32 * SECTOR, MAGIC.length), 0, MAGIC.length) === MAGIC;
}

async function findBase(blob) {
  for (const base of KNOWN_BASES) {
    if (await hasMagic(blob, base)) {
      return base;
    }
  }
  throw new Error('no Xbox 360 file system found in this image');
}

function* entries(table) {
  const seen = new Set();
  const stack = [0];
  while (stack.length) {
    const offset = stack.pop();
    if (seen.has(offset) || offset + 14 > table.length) {
      continue;
    }
    seen.add(offset);
    const view = new DataView(table.buffer, table.byteOffset, table.byteLength);
    const left = view.getUint16(offset, true);
    const right = view.getUint16(offset + 2, true);
    const sector = view.getUint32(offset + 4, true);
    const size = view.getUint32(offset + 8, true);
    const attributes = table[offset + 12];
    const length = table[offset + 13];
    for (const child of [left, right]) {
      if (child !== 0 && child !== 0xffff) {
        stack.push(child * 4);
      }
    }
    if (!length || offset + 14 + length > table.length) {
      continue;
    }
    yield { name: text(table, offset + 14, length), sector, size, directory: (attributes & 0x10) !== 0 };
  }
}

// Returns [{ path, size, read(offset, length) }] for every file, with "/" separators.
export async function listIsoFiles(blob) {
  const base = await findBase(blob);
  const descriptor = await readBytes(blob, base + 32 * SECTOR, SECTOR);
  const view = new DataView(descriptor.buffer);
  const files = [];
  const walk = async (sector, size, prefix) => {
    if (!size || size > 256 * 1024 * 1024) {
      return;
    }
    const table = await readBytes(blob, base + sector * SECTOR, size);
    const children = [...entries(table)].sort((a, b) => a.name.toLowerCase().localeCompare(b.name.toLowerCase()));
    for (const child of children) {
      const path = prefix ? `${prefix}/${child.name}` : child.name;
      if (child.directory) {
        await walk(child.sector, child.size, path);
      } else {
        const start = base + child.sector * SECTOR;
        files.push({
          path,
          size: child.size,
          read: (offset, length) => readBytes(blob, start + offset, Math.min(length, child.size - offset)),
        });
      }
    }
  };
  await walk(view.getUint32(0x14, true), view.getUint32(0x18, true), '');
  return files;
}

// Same shape for a folder chosen by the user (the extracted "XEX format" folder): File objects that carry
// webkitRelativePath. The common top folder is removed so paths start at default.xex / Movies / NFS.
export function listFolderFiles(fileList) {
  const all = [...fileList];
  const first = all.length ? (all[0].webkitRelativePath || all[0].name).split('/') : [];
  const strip = first.length > 1 ? first[0] + '/' : '';
  return all.map((file) => {
    const relative = file.webkitRelativePath || file.name;
    return {
      path: strip && relative.startsWith(strip) ? relative.slice(strip.length) : relative,
      size: file.size,
      read: async (offset, length) => new Uint8Array(await file.slice(offset, offset + length).arrayBuffer()),
    };
  });
}
