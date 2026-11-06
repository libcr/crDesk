import {hex, random} from './remote-protocol.js';
export const MAX_FILE = 64 * 1024 * 1024;
const CHUNK = 32768, HEADER = 41;
const encoder = new TextEncoder(), decoder = new TextDecoder('utf-8', {fatal:true});
const number = s => typeof s === 'string' && /^(0|[1-9][0-9]*)$/.test(s) && Number.isSafeInteger(Number(s)) ? Number(s) : -1;
// One transfer at a time, matching the native FileTransfer state machine.
export class RemoteFiles {
  constructor(channel, update, download) {
    Object.assign(this, {channel, update, download});
    this.closed = false; this.pending = 0; this.chain = Promise.resolve(); this.allowed = false;
    channel.binaryType = 'arraybuffer';
    channel.onmessage = event => {
      if ((typeof event.data === 'string' && event.data.length > 4096) ||
          (event.data instanceof ArrayBuffer && event.data.byteLength > HEADER + CHUNK)) {
        this.cancel('remoteFailed'); channel.close(); return;
      }
      if (++this.pending > 32) { this.pending--; this.cancel('remoteFailed'); return; }
      this.chain = this.chain.then(() => this.receive(event.data)).catch(() => this.cancel('remoteFailed')).finally(() => this.pending--);
    };
    channel.onclose = () => this.cancel('remoteClosed', false);
  }
  send(value) {
    if (this.closed || this.channel.readyState !== 'open' || this.channel.bufferedAmount > 512 * 1024) throw Error('channel');
    this.channel.send(value instanceof ArrayBuffer ? value : JSON.stringify(value));
  }
  arm() { clearTimeout(this.timer); this.timer = setTimeout(() => this.cancel('remoteTimeout'), 60000); }
  report(phase) {
    this.phase = phase;
    this.update({phase, name:this.task?.name || '', size:this.task?.size || 0,
      bytes:this.task?.ack || 0, receiving:this.task?.receiving || false});
  }
  cancel(reason = 'remoteCancelled', notify = true) {
    if (this.task && notify) { try { this.send({op:'cancel', id:this.task.id}); } catch {} }
    clearTimeout(this.timer); this.report(reason); this.task = null;
  }
  setAllowed(allowed) { this.allowed = allowed; if (!allowed && this.task) this.cancel(); }
  async upload(file) {
    if (this.closed || !this.allowed || this.task || this.channel.readyState !== 'open') return;
    if (file.size > MAX_FILE) { this.report('remoteFileLimit'); return; }
    if (!file.name || encoder.encode(file.name).length > 255 || /[/\\:\x00]/.test(file.name) || ['.', '..'].includes(file.name)) { this.report('remoteFailed'); return; }
    const task = {id:random(16), name:file.name, size:file.size, sent:0, ack:0, receiving:false};
    this.task = task; this.report('remotePreparing'); this.arm();
    try {
      task.data = await file.arrayBuffer();
      task.digest = hex(await crypto.subtle.digest('SHA-256', task.data));
      if (this.task !== task) return;
      this.report('remoteOffered'); this.send({op:'offer', id:task.id, name:task.name, size:String(task.size)}); this.arm();
    } catch { if (this.task === task) this.cancel('remoteFailed'); }
  }
  accept() {
    if (!this.allowed || !this.task?.receiving || this.phase !== 'remoteOffered') return;
    try { this.report('remoteTransferring'); this.send({op:'ack', id:this.task.id, offset:'0'}); this.arm(); }
    catch { this.cancel('remoteFailed'); }
  }
  async receive(data) {
    if (this.closed) return;
    const task = this.task;
    if (data instanceof ArrayBuffer) {
      if (data.byteLength < HEADER || data.byteLength > HEADER + CHUNK) throw Error('chunk');
      const bytes = new Uint8Array(data);
      if (bytes[0] === 67) return; // Clipboard is not enabled by this viewer.
      if (bytes[0] !== 70) throw Error('chunk');
      const id = decoder.decode(bytes.subarray(1, 33));
      if (!task || id !== task.id) return;
      const offset = new DataView(data).getBigUint64(33);
      if (!task.receiving || this.phase !== 'remoteTransferring' || offset !== BigInt(task.ack) ||
          data.byteLength === HEADER || task.ack + data.byteLength - HEADER > task.size) throw Error('chunk');
      task.parts.push(data.slice(HEADER)); task.ack += data.byteLength - HEADER;
      this.send({op:'ack', id, offset:String(task.ack)}); this.report('remoteTransferring'); this.arm(); return;
    }
    if (typeof data !== 'string' || data.length > 4096) throw Error('packet');
    const m = JSON.parse(data);
    if (typeof m.op !== 'string' || !/^[0-9a-fA-F]{32}$/.test(m.id)) return;
    if (m.op.startsWith('clipboard_')) return;
    if (m.op === 'offer') {
      const size = number(m.size);
      if (task || !this.allowed || size < 0 || size > MAX_FILE || typeof m.name !== 'string' || !m.name ||
          encoder.encode(m.name).length > 255 || /[/\\:\x00]/.test(m.name) || ['.', '..'].includes(m.name)) {
        this.send({op:'cancel', id:m.id});
        if (!task) this.report(size > MAX_FILE ? 'remoteFileLimit' : 'remoteCancelled'); return;
      }
      this.task = {id:m.id, name:m.name, size, ack:0, parts:[], receiving:true};
      this.report('remoteOffered'); this.arm(); return;
    }
    if (!task || m.id !== task.id) return;
    if (m.op === 'cancel') { this.cancel('remoteCancelled', false); return; }
    if (!task.receiving) {
      if (m.op === 'complete' && this.phase === 'remoteVerifying') { this.cancel('remoteComplete', false); return; }
      const offset = number(m.offset);
      if (m.op !== 'ack' || !['remoteOffered', 'remoteTransferring'].includes(this.phase) || offset < task.ack || offset > task.sent ||
          (this.phase === 'remoteTransferring' && offset === task.ack) || (offset !== task.size && offset % CHUNK)) throw Error('ack');
      task.ack = offset;
      if (offset === task.size) {
        this.send({op:'finish', id:task.id, sha256:task.digest}); task.data = null;
        this.report('remoteVerifying'); this.arm(); return;
      }
      while (task.sent < task.size && task.sent - task.ack < 4 * CHUNK) {
        const length = Math.min(CHUNK, task.size - task.sent), packet = new Uint8Array(HEADER + length);
        packet[0] = 70; packet.set(encoder.encode(task.id), 1);
        new DataView(packet.buffer).setBigUint64(33, BigInt(task.sent));
        packet.set(new Uint8Array(task.data, task.sent, length), HEADER);
        this.send(packet.buffer); task.sent += length;
      }
      this.report('remoteTransferring'); this.arm(); return;
    }
    if (m.op !== 'finish' || this.phase !== 'remoteTransferring' || task.ack !== task.size) throw Error('finish');
    this.report('remoteVerifying');
    const blob = new Blob(task.parts, {type:'application/octet-stream'});
    task.parts = [];
    const digest = hex(await crypto.subtle.digest('SHA-256', await blob.arrayBuffer()));
    if (this.task !== task) return;
    if (m.sha256 !== digest) throw Error('checksum');
    this.download(blob, task.name); this.send({op:'complete', id:task.id}); this.cancel('remoteComplete', false);
  }
  close() { this.closed = true; this.cancel('remoteClosed', false); this.channel.onmessage = null; this.channel.onclose = null; }
}
