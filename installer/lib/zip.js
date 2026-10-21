// Copied unchanged from nfsmw-nx-installer by StevensND (GPL-3.0),
// https://github.com/StevensND/nfsmw-nx-installer/blob/main/lib/zip.js
// Streaming ZIP writer (stored, no compression) for archives larger than 4 GB. Every entry must be
// smaller than 4 GB; ZIP64 records are used for offsets and for the end of the central directory.
// sink: { write(Uint8Array): Promise<void>, close(): Promise<void> }.

const CRC_TABLE = (() => {
  const table = new Uint32Array(256 * 8);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[i] = c >>> 0;
  }
  for (let i = 0; i < 256; i++) {
    for (let t = 1; t < 8; t++) {
      table[t * 256 + i] = (table[(t - 1) * 256 + i] >>> 8) ^ table[table[(t - 1) * 256 + i] & 0xff];
    }
  }
  return table;
})();

export function crc32Update(crc, data) {
  let c = ~crc >>> 0;
  let i = 0;
  const n = data.length;
  const t = CRC_TABLE;
  for (; i + 8 <= n; i += 8) {
    c ^= data[i] | (data[i + 1] << 8) | (data[i + 2] << 16) | (data[i + 3] << 24);
    c =
      t[7 * 256 + (c & 0xff)] ^ t[6 * 256 + ((c >>> 8) & 0xff)] ^ t[5 * 256 + ((c >>> 16) & 0xff)] ^
      t[4 * 256 + (c >>> 24)] ^ t[3 * 256 + data[i + 4]] ^ t[2 * 256 + data[i + 5]] ^
      t[256 + data[i + 6]] ^ t[data[i + 7]];
  }
  for (; i < n; i++) {
    c = t[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  }
  return ~c >>> 0;
}

function dosDateTime(date) {
  const time = (date.getHours() << 11) | (date.getMinutes() << 5) | Math.floor(date.getSeconds() / 2);
  const day = ((date.getFullYear() - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
  return { time, day };
}

// Exact size of the archive that ZipWriter produces for these entries, in the same order: [{ path, size }].
export function zipSize(entries) {
  const encoder = new TextEncoder();
  let offset = 0;
  let directory = 0;
  for (const e of entries) {
    const nameLength = encoder.encode(e.path).length;
    directory += 46 + nameLength + (offset >= 0xffffffff ? 12 : 0);
    offset += 30 + nameLength + e.size + 16;
  }
  return offset + directory + 56 + 20 + 22;
}

export class ZipWriter {
  constructor(sink) {
    this.sink = sink;
    this.offset = 0;
    this.entries = [];
    this.stamp = dosDateTime(new Date());
  }

  async write(bytes) {
    await this.sink.write(bytes);
    this.offset += bytes.length;
  }

  // chunks: async iterable of Uint8Array whose total length is `size`. onChunk(bytes) sees every chunk.
  async addFile(path, size, chunks, onChunk = null) {
    if (size >= 0xffffffff) {
      throw new Error(`${path} is too large for this archive`);
    }
    const name = new TextEncoder().encode(path);
    const start = this.offset;
    const header = new DataView(new ArrayBuffer(30));
    header.setUint32(0, 0x04034b50, true);
    header.setUint16(4, 20, true);
    header.setUint16(6, 0x0808, true); // data descriptor + UTF-8 names
    header.setUint16(8, 0, true);
    header.setUint16(10, this.stamp.time, true);
    header.setUint16(12, this.stamp.day, true);
    header.setUint16(26, name.length, true);
    await this.write(new Uint8Array(header.buffer));
    await this.write(name);
    let crc = 0;
    let written = 0;
    for await (const chunk of chunks) {
      crc = crc32Update(crc, chunk);
      written += chunk.length;
      if (onChunk) {
        onChunk(chunk);
      }
      await this.write(chunk);
    }
    if (written !== size) {
      throw new Error(`${path}: expected ${size} bytes, read ${written}`);
    }
    const descriptor = new DataView(new ArrayBuffer(16));
    descriptor.setUint32(0, 0x08074b50, true);
    descriptor.setUint32(4, crc, true);
    descriptor.setUint32(8, size, true);
    descriptor.setUint32(12, size, true);
    await this.write(new Uint8Array(descriptor.buffer));
    this.entries.push({ name, crc, size, start });
  }

  async addBytes(path, bytes) {
    await this.addFile(path, bytes.length, [bytes]);
  }

  async finish() {
    const directoryStart = this.offset;
    for (const e of this.entries) {
      const zip64 = e.start >= 0xffffffff;
      const extra = zip64 ? 12 : 0;
      const record = new DataView(new ArrayBuffer(46 + extra));
      record.setUint32(0, 0x02014b50, true);
      record.setUint16(4, 45, true);
      record.setUint16(6, zip64 ? 45 : 20, true);
      record.setUint16(8, 0x0808, true);
      record.setUint16(10, 0, true);
      record.setUint16(12, this.stamp.time, true);
      record.setUint16(14, this.stamp.day, true);
      record.setUint32(16, e.crc, true);
      record.setUint32(20, e.size, true);
      record.setUint32(24, e.size, true);
      record.setUint16(28, e.name.length, true);
      record.setUint16(30, extra, true);
      record.setUint32(42, zip64 ? 0xffffffff : e.start, true);
      if (zip64) {
        record.setUint16(46, 0x0001, true);
        record.setUint16(48, 8, true);
        record.setBigUint64(50, BigInt(e.start), true);
      }
      const bytes = new Uint8Array(record.buffer);
      await this.write(bytes.subarray(0, 46));
      await this.write(e.name);
      if (zip64) {
        await this.write(bytes.subarray(46));
      }
    }
    const directorySize = this.offset - directoryStart;
    const zip64End = this.offset;
    const record = new DataView(new ArrayBuffer(56 + 20 + 22));
    record.setUint32(0, 0x06064b50, true);
    record.setBigUint64(4, 44n, true);
    record.setUint16(12, 45, true);
    record.setUint16(14, 45, true);
    record.setBigUint64(24, BigInt(this.entries.length), true);
    record.setBigUint64(32, BigInt(this.entries.length), true);
    record.setBigUint64(40, BigInt(directorySize), true);
    record.setBigUint64(48, BigInt(directoryStart), true);
    record.setUint32(56, 0x07064b50, true);
    record.setBigUint64(64, BigInt(zip64End), true);
    record.setUint32(72, 1, true);
    record.setUint32(76, 0x06054b50, true);
    record.setUint16(84, Math.min(this.entries.length, 0xffff), true);
    record.setUint16(86, Math.min(this.entries.length, 0xffff), true);
    record.setUint32(88, Math.min(directorySize, 0xffffffff), true);
    record.setUint32(92, directoryStart >= 0xffffffff ? 0xffffffff : directoryStart, true);
    await this.write(new Uint8Array(record.buffer));
    await this.sink.close();
  }
}
