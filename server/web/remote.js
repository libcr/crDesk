import {RemoteProtocol, random} from './remote-protocol.js';
import {RemoteFiles} from './remote-files.js';
import {RemoteInput} from './remote-input.js';
const $ = id => document.getElementById(id);
let text = {}, flow = '', csrf = '', account = '', active = null, preparing = false, downloadUrl = '';
const t = key => text[key] || text.remoteFailed || key;
const busyFile = phase => ['remotePreparing', 'remoteOffered', 'remoteTransferring', 'remoteVerifying'].includes(phase);
const status = key => { $('status').textContent = t(key); };
let toolbarTimer = 0, toolbarHovered = false;
let targetDevice = null, avatarUrl = '', pageClosing = false;
function labelControl(element, key) {
  element.setAttribute('aria-label', t(key)); element.title = t(key);
}
function toolbarIcon(id, name, label) {
  $(id).querySelector('use').setAttribute('href', '#icon-' + name);
  labelControl($(id), label);
}
function updateDesktopCursor() {
  const video = $('desktop');
  const connected = active?.pc?.connectionState === 'connected' &&
    active?.control?.readyState === 'open' && !active?.capabilities?.paused &&
    !!video.srcObject && video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA;
  video.classList.toggle('remote-connected', connected);
}
function scheduleToolbarHide() {
  clearTimeout(toolbarTimer);
  if (!active) return;
  toolbarTimer = setTimeout(() => {
    if (toolbarHovered || $('remoteToolbar').contains(document.activeElement) ||
        !$('keyMenu').hidden || !$('resolutionMenu').hidden) return;
    $('remoteToolbar').classList.add('concealed'); $('remoteToolbar').inert = true;
    $('toolbarHandle').setAttribute('aria-expanded', 'false');
  }, 3000);
}
function showToolbar() {
  const connected = active?.control?.readyState === 'open';
  $('remoteToolbar').hidden = !connected; $('toolbarHandle').hidden = !connected;
  if (!connected) { clearTimeout(toolbarTimer); return; }
  $('remoteToolbar').inert = false; $('remoteToolbar').classList.remove('concealed');
  $('toolbarHandle').setAttribute('aria-expanded', 'true'); scheduleToolbarHide();
}
function closeToolbarMenus(restoreFocus = false) {
  for (const [menu, button] of [['keyMenu','sendKey'], ['resolutionMenu','resolution']]) {
    if ($(menu).hidden) continue;
    $(menu).hidden = true; $(button).setAttribute('aria-expanded', 'false');
    if (restoreFocus && !$(button).disabled) $(button).focus({preventScroll:true});
  }
  scheduleToolbarHide();
}
function toggleToolbarMenu(menu, button) {
  const open = $(menu).hidden; closeToolbarMenus();
  if (!open) return;
  active?.input.release(); showToolbar();
  $(menu).hidden = false; $(button).setAttribute('aria-expanded', 'true');
  $(menu).querySelector('button:not(:disabled)')?.focus({preventScroll:true});
}
function setupToolbar() {
  $('screen').addEventListener('pointermove', event => {
    if (event.pointerType !== 'mouse') return;
    const box = $('screen').getBoundingClientRect();
    if (event.clientY - box.top < 32 && Math.abs(event.clientX - box.left - box.width / 2) < 140) showToolbar();
  });
  $('remoteToolbar').addEventListener('pointerenter', e => { if (e.pointerType === 'mouse') toolbarHovered = true; showToolbar(); });
  $('remoteToolbar').addEventListener('pointerleave', () => { toolbarHovered = false; scheduleToolbarHide(); });
  $('remoteToolbar').addEventListener('focusin', showToolbar);
  $('remoteToolbar').addEventListener('focusout', scheduleToolbarHide);
  $('toolbarHandle').addEventListener('pointerenter', showToolbar);
  $('toolbarHandle').addEventListener('focus', showToolbar);
  $('toolbarHandle').onclick = () => { showToolbar(); $('remoteToolbar').querySelector('button:not(:disabled)')?.focus(); };
  $('sendKey').onclick = () => toggleToolbarMenu('keyMenu', 'sendKey');
  $('resolution').onclick = () => toggleToolbarMenu('resolutionMenu', 'resolution');
  document.addEventListener('pointerdown', event => {
    if (!$('remoteToolbar').contains(event.target)) closeToolbarMenus();
  });
  for (const menu of [$('keyMenu'), $('resolutionMenu')]) {
    menu.addEventListener('keydown', event => {
      if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); closeToolbarMenus(true); return; }
      if (event.key === 'Tab') { closeToolbarMenus(true); return; }
      if (!['ArrowDown','ArrowUp','Home','End'].includes(event.key)) return;
      event.preventDefault(); event.stopPropagation();
      const buttons = [...menu.querySelectorAll('button:not(:disabled)')], index = buttons.indexOf(document.activeElement);
      const next = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1 :
        (index + (event.key === 'ArrowDown' ? 1 : -1) + buttons.length) % buttons.length;
      buttons[next]?.focus();
    });
  }
  $('keyMenu').addEventListener('click', event => {
    const button = event.target.closest('[data-chord]'); if (!button || button.disabled) return;
    const input = active?.input;
    closeToolbarMenus(); $('desktop').focus({preventScroll:true});
    input?.chord(button.dataset.chord);
  });
  $('resolutionMenu').addEventListener('click', event => {
    const button = event.target.closest('[data-resolution]'); if (!button || !active) return;
    const mode = Number(button.dataset.resolution);
    inputSend(active, {kind:'video_resolution', mode});
    for (const item of $('resolutionMenu').children) item.setAttribute('aria-checked', String(item === button));
    closeToolbarMenus(); $('desktop').focus({preventScroll:true});
  });
}
function renderKey() {
  const input = $('secret'), focused = document.activeElement === input && !input.disabled;
  const start = input.selectionStart ?? input.value.length, end = input.selectionEnd ?? start;
  document.querySelectorAll('.key-cell').forEach((cell, index) => {
    cell.textContent = input.value[index] || '';
    cell.classList.toggle('current', focused && (start === end ? index === Math.min(start, 8) : index >= start && index < end));
  });
}
function setupKey() {
  const input = $('secret');
  const digits = value => value.replace(/[\uFF10-\uFF19]/g, digit => String.fromCharCode(digit.charCodeAt(0) - 0xFEE0)).replace(/[^0-9]/g, '');
  const changed = event => {
    if (event.isComposing) return;
    const position = digits(input.value.slice(0, input.selectionStart ?? input.value.length)).length;
    const value = digits(input.value).slice(0, 9);
    if (value !== input.value) {
      input.value = value;
      input.setSelectionRange(Math.min(position, 9), Math.min(position, 9));
    }
    renderKey();
    if (!input.disabled && input.value.length === 9) connect();
  };
  input.addEventListener('input', changed);
  input.addEventListener('compositionend', changed);
  // The transparent native input remains the only focus/keyboard/clipboard target.
  // Map pointer positions to the visible cells instead of its hidden text metrics.
  input.addEventListener('pointerdown', event => {
    if (input.disabled || event.button !== 0) return;
    event.preventDefault(); input.focus();
    const cells = [...document.querySelectorAll('.key-cell')];
    let index = cells.findIndex(cell => event.clientX < cell.getBoundingClientRect().right);
    if (index < 0) index = 8;
    index = Math.min(index, input.value.length);
    input.setSelectionRange(index, Math.min(index + 1, input.value.length));
    renderKey();
  });
  for (const name of ['focus', 'blur', 'select', 'keyup']) input.addEventListener(name, renderKey);
  document.addEventListener('selectionchange', () => { if (document.activeElement === input) renderKey(); });
  renderKey();
}
async function api(path, body = {}) {
  const response = await fetch('/auth/' + path, {method:'POST', credentials:'same-origin', cache:'no-store',
    headers:{'Content-Type':'application/json'}, body:JSON.stringify({flow, csrf, ...body}), signal:AbortSignal.timeout(15000)});
  const result = await response.json();
  if (!response.ok || result.error) throw Error(result.error || 'remoteFailed');
  return result;
}
async function showDeviceAvatar(device) {
  const image = $('deviceAvatar');
  const index = Number.isInteger(device.avatar) && device.avatar >= 0 && device.avatar <= 8 ? device.avatar : 0;
  const fallback = '/auth/avatars/' + index + '.png';
  image.onerror = () => { image.onerror = null; image.src = fallback; };
  image.src = fallback; image.hidden = false;
  image.classList.toggle('offline', !device.online);
  const hash = device.avatar_hash;
  if (!/^[a-f0-9]{64}$/.test(hash || '')) return;
  try {
    const result = await api('account/avatar', {id:device.id, hash});
    if (pageClosing || result.hash !== hash || typeof result.data !== 'string' || result.data.length > 32768 ||
        !/^[A-Za-z0-9+/]*={0,2}$/.test(result.data)) return;
    const bytes = Uint8Array.from(atob(result.data), c => c.charCodeAt(0));
    if (bytes.length < 4 || bytes.length > 24576 || bytes[0] !== 255 || bytes[1] !== 216) return;
    if (avatarUrl) URL.revokeObjectURL(avatarUrl);
    avatarUrl = URL.createObjectURL(new Blob([bytes], {type:'image/jpeg'}));
    image.src = avatarUrl;
  } catch { /* Keep the built-in avatar if the custom image is unavailable. */ }
}
async function context() {
  const result = await api('browser/start', {flow:'', management:true});
  if (!result.signed_in || (account && account !== result.account)) throw Error('sign_in_required');
  flow = result.flow; csrf = result.csrf; account = result.account;
}
let transferTimer = 0, transferComplete = false, hasTransfer = false;
function positionTransferPopup() {
  if ($('transferDetails').hidden) return;
  const button = $('upload').getBoundingClientRect(), popup = $('transferDetails').getBoundingClientRect();
  const x = Math.max(16, Math.min(popup.width - 16, button.left + button.width / 2 - popup.left));
  $('transferDetails').style.setProperty('--transfer-arrow', x + 'px');
}
function scheduleTransferHide() {
  clearTimeout(transferTimer);
  if (!transferComplete) return;
  transferTimer = setTimeout(() => {
    // Do not remove a focused Save button while someone is using the popup.
    if ($('transferDetails').contains(document.activeElement)) return;
    $('transferDetails').hidden = true;
  }, 8000);
}
function showTransferPopup() {
  if (!hasTransfer) return;
  $('transferDetails').hidden = false; positionTransferPopup(); scheduleTransferHide();
}
function setupTransferPopup() {
  const picker = $('upload').parentElement;
  picker.addEventListener('pointerenter', showTransferPopup);
  $('upload').addEventListener('focus', showTransferPopup);
  $('transferDetails').addEventListener('focusout', scheduleTransferHide);
  window.addEventListener('resize', positionTransferPopup);
  new ResizeObserver(positionTransferPopup).observe(picker);
}
function download(blob, name) {
  if (downloadUrl) URL.revokeObjectURL(downloadUrl);
  downloadUrl = URL.createObjectURL(blob);
  $('download').href = downloadUrl; $('download').download = name; $('download').hidden = false;
}
function transfer(state) {
  // Closing an idle channel should not replace the last result or its Save link.
  if (state.phase === 'remoteClosed' && !state.name) return;
  clearTimeout(transferTimer); hasTransfer = true; transferComplete = state.phase === 'remoteComplete';
  $('transfer').textContent = (state.name ? state.name + ' · ' : '') + t(state.phase) +
      (state.size ? ` · ${(state.bytes / 1048576).toFixed(1)} / ${(state.size / 1048576).toFixed(1)} MiB` : '');
  $('transfer').title = $('transfer').textContent;
  $('transferSuccess').hidden = !transferComplete;
  $('progress').hidden = !state.name;
  $('progress').value = state.size ? state.bytes / state.size : state.phase === 'remoteComplete' ? 1 : 0;
  $('accept').hidden = !(state.phase === 'remoteOffered' && state.receiving);
  $('cancel').hidden = !busyFile(state.phase);
  $('download').hidden = state.phase !== 'remoteComplete' || !state.receiving || !downloadUrl;
  $('upload').disabled = busyFile(state.phase) || !(active?.permissions & 8) || active?.fileChannel?.readyState !== 'open';
  showTransferPopup();
}
function stop(reason = 'remoteClosed') {
  const s = active; if (!s) { status(reason); return; }
  if (s.stopping) return; s.stopping = true;
  // Release keys before closing the reliable channel.
  s.input?.close(); s.secret = ''; s.ticket = '';
  if (s.ws.readyState === WebSocket.OPEN && s.id) {
    try { s.ws.send(JSON.stringify({type:'close', session_id:s.id})); } catch {}
  }
  active = null;
  for (const timer of [s.heartbeat, s.statsTimer, s.deadline, s.iceRefresh]) clearInterval(timer);
  s.files?.close(); s.protocol?.close(); s.pc?.close(); s.ws.close();
  for (const audio of s.audio) { audio.pause(); audio.srcObject = null; }
  toolbarIcon('sound', 'mute', 'remoteSound'); $('sound').setAttribute('aria-pressed', 'false');
  $('desktop').srcObject = null; $('stats').textContent = '';
  updateDesktopCursor();
  closeToolbarMenus(); showToolbar();
  for (const id of ['disconnect','fullscreen','sendKey','sound','resolution','upload']) $(id).disabled = true;
  $('secret').disabled = !targetDevice?.online;
  renderKey();
  navigator.keyboard?.unlock?.();
  status(reason);
}
function wsSend(s, m) {
  if (active !== s) return;
  if (s.ws.readyState !== WebSocket.OPEN || s.ws.bufferedAmount > 256 * 1024) { stop('remoteInterrupted'); return; }
  try { s.ws.send(JSON.stringify(m)); } catch { stop('remoteInterrupted'); }
}
function inputSend(s, value) {
  if (active !== s || s.control?.readyState !== 'open') return;
  const motion = value.kind === 'pointer' && value.button === 0;
  const channel = motion && s.motion?.readyState === 'open' ? s.motion : s.control;
  if (motion && channel.bufferedAmount > 4096) return;
  if (channel.bufferedAmount > 128 * 1024) { stop('remoteFailed'); return; }
  try { channel.send(JSON.stringify(value)); } catch { stop('remoteFailed'); }
}
function capabilities(s) {
  updateDesktopCursor();
  s.input.configure(s.capabilities, !!(s.permissions & 1));
  $('sendKey').disabled = !s.input.enabled;
  for (const button of $('keyMenu').children)
    button.disabled = !s.input.enabled || (button.dataset.chord === 'ctrl-alt-delete' && !s.input.sas);
  if (!s.input.enabled && !$('keyMenu').hidden) closeToolbarMenus();
  s.files?.setAllowed(!!(s.permissions & 8));
  $('upload').disabled = !(s.permissions & 8) || s.fileChannel?.readyState !== 'open' || busyFile(s.files?.phase);
  if (s.control?.readyState === 'open') status(s.capabilities.paused ? 'remotePaused' : s.input.enabled ? 'remoteConnected' : 'remoteViewOnly');
}
function controlMessage(s, event) {
  if (active !== s || typeof event.data !== 'string' || event.data.length > 131072) return;
  try {
    const m = JSON.parse(event.data);
    if (m.kind === 'permissions') { s.permissions = Number.isInteger(m.mask) ? m.mask & 15 : 0; capabilities(s); }
    else if (m.kind === 'capabilities') { s.capabilities = m; capabilities(s); }
    else if (m.kind === 'cursor') s.input.cursor(m);
    else if (m.kind === 'pairing' && typeof m.id === 'string') {
      // This temporary browser does not retain pairing secrets.
      inputSend(s, {kind:'pairing_result', id:m.id, ok:false});
    }
  } catch { stop('remoteFailed'); }
}
async function offer(s, m) {
  if (active !== s) return;
  if (m.type === 'candidate') {
    const candidate = {candidate:m.candidate, sdpMid:m.mid, sdpMLineIndex:m.index};
    if (typeof m.candidate !== 'string' || m.candidate.length > 8192 || !Number.isInteger(m.index)) throw Error('candidate');
    if (!s.pc.remoteDescription) {
      if (s.candidates.length >= 256) throw Error('candidate'); s.candidates.push(candidate);
    } else await s.pc.addIceCandidate(candidate);
    return;
  }
  if (m.type !== 'offer' || typeof m.sdp !== 'string' || m.sdp.length > 192000) throw Error('sdp');
  status('remoteConnecting');
  await s.pc.setRemoteDescription({type:'offer', sdp:m.sdp});
  if (active !== s) return;
  // Interoperable 8-bit codecs first. No server-side video transcoding.
  const available = RTCRtpReceiver.getCapabilities('video')?.codecs || [];
  const codecs = ['video/H264','video/VP8','video/rtx'].flatMap(mime => available.filter(c => c.mimeType.toLowerCase() === mime.toLowerCase()));
  for (const transceiver of s.pc.getTransceivers()) {
    if (transceiver.receiver.track.kind === 'video' && codecs.some(c => /h264|vp8/i.test(c.mimeType))) transceiver.setCodecPreferences?.(codecs);
  }
  for (const candidate of s.candidates.splice(0)) await s.pc.addIceCandidate(candidate);
  const answer = await s.pc.createAnswer();
  if (active !== s) return;
  await s.pc.setLocalDescription(answer);
  await s.protocol.answer(s.pc.localDescription.sdp);
}
function makePeer(s, servers) {
  s.pc = new RTCPeerConnection({iceServers:servers, bundlePolicy:'max-bundle'});
  s.pc.onicecandidate = event => {
    if (active !== s || !event.candidate) return;
    s.protocol.send({type:'candidate', candidate:event.candidate.candidate, mid:event.candidate.sdpMid, index:event.candidate.sdpMLineIndex});
  };
  s.pc.onconnectionstatechange = () => {
    if (active !== s) return;
    updateDesktopCursor();
    if (s.pc.connectionState === 'failed') stop('remoteFailed');
    else if (s.pc.connectionState === 'disconnected') status('remoteInterrupted');
    else if (s.pc.connectionState === 'connected') capabilities(s);
  };
  s.pc.ontrack = event => {
    if (active !== s) return;
    if (event.track.kind === 'video') {
      $('desktop').srcObject = new MediaStream([event.track]); $('desktop').play().catch(() => {});
    } else {
      const audio = new Audio(); audio.srcObject = new MediaStream([event.track]); audio.muted = !s.sound;
      s.audio.push(audio); if (s.sound) audio.play().catch(() => status('remotePlaySound'));
    }
  };
  s.pc.ondatachannel = event => {
    if (active !== s) { event.channel.close(); return; }
    const c = event.channel;
    if (c.label === 'crdesk-input-v1' && !s.control) {
      s.control = c; c.onmessage = e => controlMessage(s, e);
      c.onopen = () => {
        if (active !== s) return;
        clearTimeout(s.deadline); capabilities(s);
        for (const id of ['fullscreen','sound']) $(id).disabled = false;
        $('resolution').disabled = !s.protocol.features?.includes('video_resolution');
        if (s.protocol.features?.includes('video_resolution')) inputSend(s, {kind:'video_resolution',
          mode:Number($('resolutionMenu').querySelector('[aria-checked=true]').dataset.resolution)});
        showToolbar();
        if (s.protocol.features?.includes('video_visibility')) inputSend(s, {kind:'video_visibility', visible:!document.hidden});
        wsSend(s, {type:'session_report', session_id:s.id, state:'connected'});
      };
      c.onclose = () => { if (active === s) stop('remoteClosed'); };
      if (c.readyState === 'open') c.onopen();
    } else if (c.label === 'crdesk-motion-v1' && !s.motion) {
      s.motion = c;
      c.onmessage = e => {
        if (active !== s || typeof e.data !== 'string' || e.data.length > 4096 ||
            !s.protocol.features?.includes('cursor_motion')) return;
        try {
          const m = JSON.parse(e.data);
          if (m.kind === 'cursor' && m.cursor_position_only === true && m.width === undefined && m.pixels === undefined)
            s.input.cursor(m);
        } catch { /* Discard malformed unordered updates. */ }
      };
    } else if (c.label === 'crdesk-files-v1' && !s.files) {
      s.fileChannel = c; s.files = new RemoteFiles(c, transfer, download);
      const closed = c.onclose; c.onclose = () => { closed(); if (active === s) capabilities(s); };
      c.onopen = () => { if (active === s) capabilities(s); };
      s.files.setAllowed(!!(s.permissions & 8));
      if (c.readyState === 'open') c.onopen();
    } else c.close();
  };
  s.statsTimer = setInterval(async () => {
    try {
      const stats = await s.pc.getStats(); if (active !== s || s.control?.readyState !== 'open') return;
      let route = '', video = '', fps = '';
      stats.forEach(r => {
        if (r.type === 'transport' && r.selectedCandidatePairId) {
          const pair = stats.get(r.selectedCandidatePairId);
          route = [stats.get(pair?.localCandidateId), stats.get(pair?.remoteCandidateId)].some(c => c?.candidateType === 'relay') ? 'relay' : 'direct';
        }
        if (r.type === 'inbound-rtp' && r.kind === 'video') {
          video = `${r.frameWidth || 0} × ${r.frameHeight || 0}`;
          fps = `${Math.round(r.framesPerSecond || 0)} FPS`;
        }
      });
      $('stats').textContent = [video, route ? t(route === 'relay' ? 'remoteRelay' : 'remoteDirect') : '', fps].filter(Boolean).join(' · ');
      if (route && route !== s.route) { s.route = route; wsSend(s, {type:'session_report', session_id:s.id, state:'connected', route}); }
    } catch {}
  }, 2000);
}
async function socketMessage(s, data) {
  if (active !== s || typeof data !== 'string' || data.length > 256 * 1024) return;
  const m = JSON.parse(data); s.lastMessage = Date.now();
  if (m.type === 'challenge') {
    if (s.authSent) throw Error('protocol'); s.authSent = true;
    wsSend(s, {type:'authenticate_browser', ticket:s.ticket}); s.ticket = ''; return;
  }
  if (m.type === 'ready') {
    if (s.requested) throw Error('protocol'); s.requested = true;
    wsSend(s, {type:'connect', request_id:s.request, target:s.target}); return;
  }
  if (m.type === 'pending' && m.request_id === s.request && !s.id) { s.id = m.session_id; return; }
  if (m.type === 'error') { stop('remote_' + m.code); return; }
  if (m.type === 'pong') return;
  if (!s.id || m.session_id !== s.id) return;
  if (m.type === 'closed') { stop(m.reason === 'sharing_stopped' ? 'remoteSharingStopped' : 'remote_' + m.reason); return; }
  if (m.type === 'accepted' && !s.accepted) {
    s.accepted = true; wsSend(s, {type:'ice_servers', session_id:s.id}); return;
  }
  if (m.type === 'ice_servers') {
    if (!s.accepted || !Array.isArray(m.servers) || m.servers.length > 16) throw Error('ice');
    const servers = m.servers.map(({urls, username, credential}) => ({urls, username, credential}));
    if (s.pc) s.pc.setConfiguration({...s.pc.getConfiguration(), iceServers:servers});
    else {
      makePeer(s, servers);
      s.protocol = new RemoteProtocol(payload => wsSend(s, {type:'signal', session_id:s.id, payload}), status,
        payload => offer(s, payload), reason => { if (active === s) stop(reason); });
      await s.protocol.start(s.secret); s.secret = '';
    }
    clearTimeout(s.iceRefresh);
    const ttl = Math.min(600, ...m.servers.map(server => server.ttl_seconds || 600));
    s.iceRefresh = setTimeout(() => { if (active === s) wsSend(s, {type:'ice_servers', session_id:s.id}); }, Math.max(15, ttl / 2) * 1000);
  } else if (m.type === 'signal') {
    if (m.from !== s.target || !s.protocol) throw Error('protocol'); s.protocol.receive(m.payload);
  }
}
async function connect(event) {
  event?.preventDefault(); if (active || preparing || $('secret').disabled) return;
  const target = targetDevice?.id, secret = $('secret').value;
  if (!target || !/^[0-9]{9}$/.test(secret)) { status('remoteKeyHint'); return; }
  preparing = true; $('secret').disabled = true;
  renderKey(); status('remoteConnecting');
  try {
    await context(); const {ticket} = await api('remote/ticket');
    const url = new URL('/auth/remote/ws', location.href); url.protocol = 'wss:';
    const s = {ws:new WebSocket(url), ticket, secret, target, request:random(16), id:'', candidates:[], permissions:0,
      capabilities:{}, audio:[], sound:false, lastMessage:Date.now(), pending:0, chain:Promise.resolve()};
    active = s; $('secret').value = ''; $('secret').disabled = true; $('disconnect').disabled = false;
    renderKey();
    s.input = new RemoteInput($('desktop'), $('cursor'), value => inputSend(s, value));
    s.ws.onmessage = event => {
      if (++s.pending > 256) { stop('remoteFailed'); return; }
      s.chain = s.chain.then(() => socketMessage(s, event.data)).catch(() => { if (active === s) stop('remoteFailed'); }).finally(() => s.pending--);
    };
    s.ws.onclose = () => { if (active === s) stop('remoteInterrupted'); };
    s.ws.onerror = () => { if (active === s) stop('remoteFailed'); };
    s.deadline = setTimeout(() => { if (active === s) stop('remoteTimeout'); }, 90000);
    s.heartbeat = setInterval(() => {
      if (active !== s) return;
      if (Date.now() - s.lastMessage > 45000) { stop('remoteTimeout'); return; }
      if (s.ws.readyState === WebSocket.OPEN) wsSend(s, {type:'ping'});
    }, 15000);
  } catch (error) {
    if (active) stop('remoteFailed');
    else if (['sign_in_required','expired'].includes(error.message)) { $('login').hidden = false; status('remoteSignIn'); }
    else status('remote_' + error.message);
  }
  finally {
    preparing = false;
    if (!active) { $('secret').disabled = !targetDevice?.online; renderKey(); }
  }
}
async function start() {
  setupKey();
  for (const event of ['loadeddata', 'playing', 'emptied'])
    $('desktop').addEventListener(event, updateDesktopCursor);
  const all = await (await fetch('/auth/i18n.json', {cache:'no-store'})).json();
  let language = 'en';
  for (const preference of navigator.languages || [navigator.language]) {
    const tag = preference.toLowerCase(), match = Object.keys(all).find(k => k.toLowerCase() === tag) ||
      (tag.startsWith('zh') ? (tag.includes('hk') ? 'zh-HK' : tag.includes('hant') || tag.includes('tw') ? 'zh-TW' : 'zh-CN') :
        tag.startsWith('pt') ? 'pt-PT' : Object.keys(all).find(k => k === tag.split('-')[0]));
    if (match) { language = match; break; }
  }
  text = {...all.en, ...all[language]}; document.documentElement.lang = language; document.documentElement.dir = language === 'ar' ? 'rtl' : 'ltr';
  document.querySelectorAll('[data-text]').forEach(el => { el.textContent = t(el.dataset.text); });
  $('fileHint').title = t('remoteFileHint');
  document.querySelectorAll('[data-label]').forEach(el => labelControl(el, el.dataset.label));
  $('desktop').setAttribute('aria-label', t('remoteTitle'));
  setupToolbar();
  setupTransferPopup();
  $('connectForm').addEventListener('submit', connect);
  $('disconnect').onclick = () => stop();
  $('sound').onclick = async () => {
    const s = active; if (!s) return; s.sound = !s.sound;
    toolbarIcon('sound', s.sound ? 'sound' : 'mute', s.sound ? 'remoteMute' : 'remoteSound');
    $('sound').setAttribute('aria-pressed', String(s.sound));
    for (const audio of s.audio) { audio.muted = !s.sound; if (s.sound) await audio.play().catch(() => status('remotePlaySound')); }
  };
  $('fullscreen').onclick = async () => {
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else { await $('screen').requestFullscreen(); $('desktop').focus(); await navigator.keyboard?.lock?.(); }
    } catch { status('remoteKeyboardHint'); }
  };
  document.addEventListener('fullscreenchange', () => {
    active?.input.release(); if (!document.fullscreenElement) navigator.keyboard?.unlock?.();
    toolbarIcon('fullscreen', document.fullscreenElement ? 'exit-fullscreen' : 'fullscreen',
      document.fullscreenElement ? 'remoteExitFullscreen' : 'remoteFullscreen');
    closeToolbarMenus(); showToolbar();
  });
  document.addEventListener('visibilitychange', () => {
    if (active?.protocol?.features?.includes('video_visibility')) inputSend(active, {kind:'video_visibility', visible:!document.hidden});
  });
  $('upload').onchange = () => { const file = $('upload').files[0]; $('upload').value = ''; if (file) active?.files?.upload(file); };
  $('accept').onclick = () => active?.files?.accept(); $('cancel').onclick = () => active?.files?.cancel();
  window.addEventListener('pagehide', () => {
    pageClosing = true; stop();
    if (downloadUrl) URL.revokeObjectURL(downloadUrl);
    if (avatarUrl) URL.revokeObjectURL(avatarUrl);
  });
  if (!isSecureContext || !window.RTCPeerConnection || !crypto.subtle) { status('remoteUnsupported'); return; }
  try {
    await context(); const records = await api('account/list');
    const target = new URL(location.href).searchParams.get('device');
    targetDevice = records.devices.find(device => device.id === target) || null;
    $('device').textContent = targetDevice?.name || targetDevice?.id || '';
    $('device').title = $('device').textContent;
    $('device').hidden = !targetDevice;
    if (targetDevice) void showDeviceAvatar(targetDevice);
    $('secret').disabled = !targetDevice?.online;
    status(targetDevice?.online ? 'remoteKeyHint' : 'remote_unavailable');
  } catch (error) {
    $('login').hidden = false;
    status(['sign_in_required','expired'].includes(error.message) ? 'remoteSignIn' : 'remoteFailed');
  }
}
start().catch(() => { $('status').textContent = 'Unable to load crDesk. Reload this page.'; });
