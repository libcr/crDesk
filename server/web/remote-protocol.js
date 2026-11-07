// crDesk protocol v4. Credentials stay in this page's memory, never in URLs/storage.
export const hex = bytes => Array.from(new Uint8Array(bytes), b => b.toString(16).padStart(2, '0')).join('').toUpperCase();
export const random = count => hex(crypto.getRandomValues(new Uint8Array(count)));
const utf8 = new TextEncoder();
const features = ['cursor', 'cursor_motion', 'input_epoch', 'sequence', 'motion', 'video_resolution', 'video_visibility', 'codec_renegotiation'];
export class RemoteProtocol {
  constructor(relay, status, offer, stopped) {
    Object.assign(this, {relay, status, offer, stopped});
    this.pending = 0; this.tx = 0n; this.rx = 0n; this.state = 'new';
    this.outgoing = Promise.resolve(); this.incoming = Promise.resolve();
  }
  async start(secret) {
    this.key = await crypto.subtle.importKey('raw', utf8.encode(secret), {name:'HMAC', hash:'SHA-256'}, false, ['sign', 'verify']);
    if (this.state === 'closed') return;
    this.client = random(32); this.profile = JSON.stringify({name:'Web browser', avatar:0, persistent_pairing:false});
    this.state = 'challenge';
    this.relay(JSON.stringify({hello:this.client, version:4, identity:'', profile:this.profile}));
  }
  transcript(role, payload) {
    return utf8.encode(`crdesk/4\n\n${this.client}\n${this.server}\n${this.profile}\n${this.hostProfile}\n${role}\n${payload}`);
  }
  async sign(role, payload) { return hex(await crypto.subtle.sign('HMAC', this.key, this.transcript(role, payload))); }
  async verify(role, payload, proof) {
    if (typeof proof !== 'string' || !/^[0-9A-F]{64}$/.test(proof)) return false;
    const bytes = Uint8Array.from(proof.match(/../g), b => parseInt(b, 16));
    return crypto.subtle.verify('HMAC', this.key, bytes, this.transcript(role, payload));
  }
  receive(text) {
    if (this.state === 'closed') return;
    if (++this.pending > 256) { this.pending--; this.stopped('remoteFailed'); return; }
    this.incoming = this.incoming.then(() => this.handle(text)).catch(() => this.stopped('remoteFailed')).finally(() => this.pending--);
  }
  async handle(text) {
    if (this.state === 'closed') return;
    if (typeof text !== 'string' || text.length > 220000) throw Error('protocol');
    const m = JSON.parse(text);
    if (this.state === 'challenge') {
      if (!/^[0-9a-fA-F]{64}$/.test(m.challenge) || typeof m.profile !== 'string' || utf8.encode(m.profile).length > 1024) throw Error('protocol');
      const profile = JSON.parse(m.profile);
      if (typeof profile.name !== 'string' || !profile.name || !Number.isInteger(profile.avatar)) throw Error('protocol');
      this.server = m.challenge; this.hostProfile = m.profile;
      const proof = await this.sign('client', 'authenticate');
      if (this.state === 'closed') return;
      this.state = 'proof'; this.relay(JSON.stringify({proof})); return;
    }
    if (this.state === 'proof') {
      if (!await this.verify('host', 'authenticate', m.proof)) return this.stopped('remote_authentication_failed');
      if (this.state === 'closed') return;
      this.state = 'open'; this.status('remoteApproval');
      this.heartbeat = setInterval(() => this.send('ping'), 5000); return;
    }
    if (this.state !== 'open' || m.seq !== String(this.rx) || typeof m.payload !== 'string' ||
        !await this.verify('host-message', m.seq + '\n' + m.payload, m.proof)) throw Error('protocol');
    if (this.state === 'closed') return;
    this.rx++;
    if (m.payload === 'ping') return;
    const payload = JSON.parse(m.payload);
    if (payload.type === 'waiting') { this.status('remoteApproval'); return; }
    if (payload.type === 'close') { this.stopped(payload.reason === 'sharing_stopped' ? 'remoteSharingStopped' : 'remoteClosed'); return; }
    if (payload.type === 'error') { this.stopped('remoteFailed'); return; }
    if (payload.type === 'crdesk_avatar' || payload.type === 'crdesk_avatar_request') return;
    if (payload.type === 'offer') {
      if (payload.protocol_major !== 4 || !Array.isArray(payload.capabilities) ||
          !['cursor', 'input_epoch', 'sequence'].every(c => payload.capabilities.includes(c))) throw Error('protocol');
      this.features = features.filter(c => payload.capabilities.includes(c));
    }
    await this.offer(payload);
  }
  send(value) {
    this.outgoing = this.outgoing.then(async () => {
      if (this.state !== 'open') return;
      const payload = typeof value === 'string' ? value : JSON.stringify(value);
      const seq = String(this.tx++), proof = await this.sign('client-message', seq + '\n' + payload);
      if (this.state === 'open') this.relay(JSON.stringify({seq, payload, proof}));
    }).catch(() => this.stopped('remoteFailed'));
    return this.outgoing;
  }
  answer(sdp) { return this.send({type:'answer', sdp, protocol_major:4, capabilities:this.features}); }
  close() { clearInterval(this.heartbeat); this.state = 'closed'; this.key = null; }
}
