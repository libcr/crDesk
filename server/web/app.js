'use strict';
(() => {
  const $ = id => document.getElementById(id);
  let flow = new URL(location.href).searchParams.get('flow');
  const currentDevice = new URL(location.href).searchParams.get('device') || '';
  let signedIn = false, management = !flow, accountId = '';
  let recovering = null;
  // Until the server identifies a flow, never discard it to start management.
  let enrollmentFlow = Boolean(flow) && new URL(location.href).searchParams.get('context') !== 'management';
  let awaitingNative = false;
  let backgroundTask = null;
  const avatarUrls = new Map();
  let avatarAbort = null;
  let text = {}, csrf = '', busy = false, recordsCache = null, editingDevice = null;
  const say = key => { $('status').textContent = text[key] || text.error || key; };
  function failed(error) {
    say(error.name === 'NotAllowedError' ? 'cancelled' : (text[error.message] ? error.message : 'error'));
    if (error.message === 'expired' || error.message === 'already_open' || error.message === 'device_revoked') {
      awaitingNative = false;
      clearAvatars(); recordsCache = null; $('devices').replaceChildren();
      signedIn = false; $('account').hidden = true; $('login').hidden = true;
      $('restart').hidden = enrollmentFlow;
      $('nativeRestartHint').hidden = !enrollmentFlow;
    }
  }
  async function requestApi(path, body = {}, signal) {
    const response = await fetch('/auth/' + path, {method:'POST', headers:{'Content-Type':'application/json'}, credentials:'same-origin', cache:'no-store', signal, body:JSON.stringify({flow, ...body, csrf})});
    const result = await response.json();
    if (!response.ok || result.error) {
      const error = new Error(result.error || 'error'); error.status = response.status; throw error;
    }
    return result;
  }
  async function api(path, body = {}, signal) {
    try { return await requestApi(path, body, signal); }
    catch (error) {
      // Only an explicit pre-dispatch context/CSRF rejection is retryable.
      // Never replay an uncertain mutation or revive an expired enrollment.
      if (path === 'browser/start' || path === 'passkey/finish' || !signedIn || !management ||
          error.status !== 403 || error.message !== 'expired' || signal?.aborted) throw error;
      if (!recovering) {
        const expectedAccount = accountId;
        recovering = requestApi('browser/start', {flow:flow || '', device:currentDevice, management:true}).then(state => {
          if (!state.management || !state.signed_in || state.account !== expectedAccount) throw new Error('expired');
          applyState(state);
        }).finally(() => { recovering = null; });
      }
      await recovering;
      return requestApi(path, body, signal);
    }
  }
  const decode = value => Uint8Array.from(atob(value.replace(/-/g,'+').replace(/_/g,'/')), c => c.charCodeAt(0));
  const encode = value => btoa(String.fromCharCode(...new Uint8Array(value))).replace(/\+/g,'-').replace(/\//g,'_').replace(/=+$/,'');
  async function run(callback, background = false) {
    // A user action waits for an in-flight refresh instead of being discarded.
    if (!background && backgroundTask) await backgroundTask;
    if (busy) return;
    busy = true;
    if (!background) document.querySelectorAll('button').forEach(b => { b.disabled = true; });
    try { await callback(); }
    catch (error) { failed(error); }
    finally {
      busy = false;
      if (!background) document.querySelectorAll('button').forEach(b => { b.disabled = b.dataset.locked === 'true'; });
    }
  }
  function refreshQuietly(callback) {
    if (busy || backgroundTask) return;
    backgroundTask = run(callback, true).finally(() => { backgroundTask = null; });
  }
  function action(id, callback) { $(id).onclick = () => run(callback); }
  function updateNextStep(state) {
    // Management is not enrollment. Wait for the account's device list before
    // showing onboarding, so registered accounts never flash this prompt.
    $('nextStep').hidden = state.management && (!recordsCache || recordsCache.devices.length > 0);
    $('approval').hidden = state.management || state.approved;
    const key = state.management ? 'manageEnrollmentHint' : !state.approved ? 'enrollDeviceHint' :
        state.claimed ? 'registrationComplete' : state.restoring ? 'restored' : 'done';
    $('nextStepText').textContent = text[key];
  }
  function applyState(state) {
    if (accountId && accountId !== (state.account || '')) {
      clearAvatars(); recordsCache = null; editingDevice = null;
      $('devices').replaceChildren(); $('keys').replaceChildren();
      $('metadataDialog').close();
    }
    csrf = state.csrf; flow = state.flow; management = state.management; signedIn = state.signed_in;
    accountId = state.account || '';
    enrollmentFlow = !management;
    awaitingNative = signedIn && !management && state.approved && !state.claimed;
    // Preserve the server-issued flow across OAuth callbacks and reloads.
    const url = new URL(location.href); url.searchParams.set('flow', flow); url.searchParams.delete('error');
    // This UI-only hint preserves recovery guidance across reloads. The server
    // remains authoritative for the flow type and all enrollment permissions.
    url.searchParams.set('context', management ? 'management' : 'device');
    history.replaceState(null, '', url);
    $('intro').textContent = management ? text.manageIntro : text.intro;
    $('device').hidden = management;
    $('device').textContent = text.device + ': ' + state.name;
    $('login').hidden = signedIn; $('account').hidden = !signedIn;
    $('google').hidden = !state.google; $('restart').hidden = true; $('nativeRestartHint').hidden = true;
    if (signedIn) {
      $('identity').textContent = state.email || state.account;
      $('loginMethod').textContent = text.loginMethod + ': ' + (state.login_method === 'google' ? 'Google' : text.passkeyName);
      updateNextStep(state);
      say(!management && state.approved ? (state.claimed ? 'registrationComplete' : state.restoring ? 'restored' : 'done') : 'signedIn');
    }
  }
  async function refresh() {
    const state = await api('browser/start', {flow:flow || '', device:currentDevice, management:!enrollmentFlow});
    applyState(state);
    if (signedIn) await refreshRecords();
  }
  async function refreshRecords() {
    const records = await api('account/list');
    const methods = [];
    if (records.google_linked) methods.push('Google');
    if (records.passkeys.length) methods.push(text.keysTitle + ' (' + records.passkeys.length + ')');
    $('loginMethods').textContent = text.loginMethods + ': ' + methods.join(', ');
    const hashes = new Set(records.devices.map(row => row.avatar_hash));
    for (const [hash, url] of avatarUrls) if (!hashes.has(hash)) { URL.revokeObjectURL(url); avatarUrls.delete(hash); }
    const devicesChanged = !recordsCache || JSON.stringify(records.devices) !== JSON.stringify(recordsCache.devices);
    const keysChanged = !recordsCache || records.google_linked !== recordsCache.google_linked ||
      JSON.stringify(records.passkeys) !== JSON.stringify(recordsCache.passkeys);
    recordsCache = records;
    if (management) $('nextStep').hidden = records.devices.length > 0;
    if (devicesChanged) renderDevices();
    if (keysChanged) render('keys', records.passkeys, 'passkey', records.google_linked);
    await refreshPanels();
  }
  function renderDevices() {
    if (!recordsCache) return;
    const selected = $('groupFilter').options.length ? $('groupFilter').value : '*', query = $('deviceSearch').value.trim().toLocaleLowerCase();
    const groups = [...new Set(recordsCache.devices.map(row => row.group || ''))].sort((a,b) => a.localeCompare(b));
    $('groupFilter').replaceChildren(new Option(text.allGroups, '*'));
    $('knownGroups').replaceChildren();
    for (const group of groups) {
      $('groupFilter').append(new Option(group || text.ungrouped, 'group:' + group));
      if (group) $('knownGroups').append(new Option(group, group));
    }
    $('groupFilter').value = selected === '*' || (selected.startsWith('group:') && groups.includes(selected.slice(6))) ? selected : '*';
    const rows = recordsCache.devices.filter(row =>
      ($('groupFilter').value === '*' || (row.group || '') === $('groupFilter').value.slice(6)) &&
      [row.name,row.group,row.note,...(row.tags || [])].join(' ').toLocaleLowerCase().includes(query));
    rows.sort((a,b) => (a.group || '').localeCompare(b.group || '') || (a.order || 0) - (b.order || 0) || a.name.localeCompare(b.name));
    loadAvatars(render('devices', rows, 'device', recordsCache.google_linked));
  }
  function clearAvatars() {
    avatarAbort?.abort(); avatarAbort = null;
    for (const url of avatarUrls.values()) URL.revokeObjectURL(url);
    avatarUrls.clear();
  }
  async function loadAvatars(images) {
    avatarAbort?.abort();
    const controller = new AbortController(); avatarAbort = controller;
    // Sequential, paced downloads leave room for account actions under the
    // reverse proxy's API rate limit. Search/refresh cancels stale requests.
    for (const {row, image} of images) {
      if (controller.signal.aborted || !signedIn) return;
      const hash = row.avatar_hash;
      if (!/^[a-f0-9]{64}$/.test(hash || '')) continue;
      if (avatarUrls.has(hash)) { image.src = avatarUrls.get(hash); continue; }
      const deadline = setTimeout(() => controller.abort(), 10000);
      try {
        const result = await api('account/avatar', {id:row.id, hash}, controller.signal);
        if (controller.signal.aborted || !signedIn) return;
        if (result.hash !== hash || typeof result.data !== 'string' || result.data.length > 32768 ||
            !/^[A-Za-z0-9+/]*={0,2}$/.test(result.data)) continue;
        const bytes = Uint8Array.from(atob(result.data), c => c.charCodeAt(0));
        if (bytes.length < 4 || bytes.length > 24576 || bytes[0] !== 255 || bytes[1] !== 216) continue;
        const url = URL.createObjectURL(new Blob([bytes], {type:'image/jpeg'}));
        if (avatarUrls.size >= 128) { URL.revokeObjectURL(url); continue; }
        avatarUrls.set(hash, url); image.src = url;
      } catch { /* Keep the built-in avatar if unavailable or the session expired. */ }
      finally { clearTimeout(deadline); }
      await new Promise(resolve => setTimeout(resolve, 200));
    }
  }
  function editMetadata(row) {
    editingDevice = row.id;
    $('metadataTitle').textContent = text.editDetails + ': ' + row.name;
    $('metadataName').value = row.name;
    $('metadataGroup').value = row.group || ''; $('metadataTags').value = (row.tags || []).join(', ');
    $('metadataNote').value = row.note || ''; $('metadataOrder').value = row.order || 0;
    $('metadataError').textContent = ''; $('metadataDialog').showModal();
  }
  function detailList(target, entries) {
    $(target).replaceChildren();
    for (const [name, value] of entries) {
      const term = document.createElement('dt'), description = document.createElement('dd');
      term.textContent = name; description.textContent = value; $(target).append(term, description);
    }
  }
  async function refreshPanels() {
    if ($('serverPanel').open) {
      const state = await api('account/status');
      const entries = [[text.revision,state.revision], [text.uptime,new Intl.NumberFormat(document.documentElement.lang, {style:'unit', unit:'minute', unitDisplay:'short'}).format(Math.floor(state.uptime / 60))],
        [text.onlineDevices,state.online], [text.signalingSessions,state.sessions],
        ['STUN',state.stun ? text.configured : text.unknown], ['TURN',state.turn.join(', ') || text.unknown]];
      for (const key of ['https','google']) {
        const probe = state.dependencies[key];
        const title = key === 'https' ? 'HTTPS / TLS' : text.googleKeys;
        entries.push([title,probe ? (probe.ok ? text.passed : text.failed) + ' · ' + when(probe.time) +
          (probe.ok ? '' : ' · net=' + probe.net_error + ' HTTP=' + probe.http_status) : text.unknown]);
        if (probe && probe.certificate_expiry) entries.push([title + ' · ' + text.certificateExpiry,when(probe.certificate_expiry)]);
      }
      detailList('serverStatus', entries);
      $('diagnostics').replaceChildren();
      if (!state.diagnostics.length) { const li = document.createElement('li'); li.textContent = text.empty; $('diagnostics').append(li); }
      for (const report of state.diagnostics) {
        const li = document.createElement('li'), details = document.createElement('div'); details.className = 'details';
        const title = document.createElement('strong'); title.textContent = report.name + ' · ' + when(report.time); details.append(title);
        for (const row of report.results) {
          const line = document.createElement('small');
          const stages = {cloud:'Cloud TLS',discovery:text.devicesTitle,ice_config:'ICE',stun:'STUN',turn_udp:'TURN UDP',turn_tcp:'TURN TCP',turn_tls:'TURN TLS'};
          line.textContent = (stages[row.stage] || row.stage) + (Number.isInteger(row.probe) ? ' #' + (row.probe + 1) : '') + ': ' +
            (row.result === 'passed' ? text.passed : row.result === 'empty_devices' ? text.empty : ['unconfigured','running','cancelled'].includes(row.result) ? text.unknown : text.failed + ' [' + row.result + ']');
          for (const key of ['error_code','net_error','http_status','relay_endpoints','side0_error','side1_error'])
            if (Number.isInteger(row[key]) && (row[key] || key === 'relay_endpoints')) line.textContent += ' · ' + key + '=' + row[key];
          details.append(line);
        }
        li.append(details); $('diagnostics').append(li);
      }
    }
    if ($('historyPanel').open) {
      const state = await api('account/history'); $('history').replaceChildren();
      if (!state.events.length) { const li = document.createElement('li'); li.textContent = text.empty; $('history').append(li); }
      for (const event of state.events) {
        const li = document.createElement('li'), details = document.createElement('div'); details.className = 'details';
        const title = document.createElement('strong');
        title.textContent = (text[event.category] || event.category) + ' · ' + (text['ev_' + event.action] || event.action);
        const subject = document.createElement('small'); subject.textContent = event.subject || '';
        const time = document.createElement('small'); time.textContent = when(event.time);
        details.append(title,subject,time);
        if (event.detail) { const detail = document.createElement('small'); detail.textContent = event.detail === 'client_report' ? text.clientReported : event.detail.startsWith('client_') ? text.clientReported + ': ' + event.detail.slice(7) : (text['reason_' + event.detail] || event.detail); details.append(detail); }
        if (event.session) { const id = document.createElement('small'); id.textContent = 'ID: ' + event.session; details.append(id); }
        li.append(details); $('history').append(li);
      }
    }
  }
  function when(value) {
    return typeof value === 'number' && Number.isFinite(value) && value > 0 ? new Date(value).toLocaleString() : text.unknown;
  }
  function render(target, rows, kind, googleLinked) {
    $(target).replaceChildren();
    const images = [];
    if (!rows.length) { const empty = document.createElement('li'); empty.textContent = text.empty; $(target).append(empty); }
    rows.forEach((row, i) => {
      const item = document.createElement('li'), details = document.createElement('div'); details.className = 'details';
      const label = document.createElement('strong');
      const name = row.name || (kind === 'device' ? text.device : text.passkeyName) + ' ' + (i + 1);
      label.textContent = name;
      const heading = document.createElement('div'); heading.className = 'device-heading';
      heading.append(label); details.append(heading);
      const info = [];
      if (kind === 'device') {
        item.className = 'device-row';
        const image = document.createElement('img'); image.className = 'device-avatar';
        image.width = image.height = 36; image.alt = ''; image.decoding = 'async';
        const index = Number.isInteger(row.avatar) && row.avatar >= 0 && row.avatar <= 8 ? row.avatar : 0;
        const fallback = '/auth/avatars/' + index + '.png';
        image.src = fallback;
        image.onerror = () => { image.onerror = null; image.src = fallback; };
        item.append(image); images.push({row, image});
        if (row.current) {
          const badge = document.createElement('span'); badge.className = 'current-device';
          badge.textContent = text.currentDevice; heading.append(badge);
        }
        info.push(row.platform || text.unknown, row.online ? text.online : text.offline);
        info.push(text.lastSeen + ': ' + (row.online ? text.online : when(row.last_seen)));
        if (row.group) info.push(text.groupLabel + ': ' + row.group);
        if (row.tags?.length) info.push(text.tagsLabel + ': ' + row.tags.join(', '));
        if (row.note) info.push(row.note);
      } else info.push(text.lastUsed + ': ' + when(row.last_used));
      info.push(text.created + ': ' + when(row.created));
      for (const line of info) { const p = document.createElement('small'); p.textContent = line; details.append(p); }
      const actions = document.createElement('div'); actions.className = 'row-actions';
      const remove = document.createElement('button');
      remove.textContent = kind === 'device' ? text.revoke : text.remove;
      remove.setAttribute('aria-label', remove.textContent + ': ' + name);
      const locked = kind === 'passkey' && rows.length === 1 && !googleLinked;
      remove.dataset.locked = String(locked); remove.disabled = locked;
      if (locked) { const note = document.createElement('small'); note.textContent = text.last_login_method; details.append(note); }
      remove.onclick = () => run(async () => {
        if (!confirm(kind === 'device' && row.current ? text.confirmCurrent : text.confirmRemove)) return;
        await api('account/remove', {kind, id:row.id}); await refreshRecords(); say('removed');
      });
      if (kind === 'device') {
        if (!row.current) {
          const connect = document.createElement('button'); connect.textContent = text.remoteBrowserAccess;
          connect.setAttribute('aria-label', text.remoteBrowserAccess + ': ' + name);
          connect.onclick = () => window.open('/auth/remote.html?device=' + encodeURIComponent(row.id), '_blank', 'noopener');
          actions.append(connect);
        }
        const edit = document.createElement('button'); edit.textContent = text.editDetails;
        edit.setAttribute('aria-label', text.editDetails + ': ' + name);
        edit.onclick = () => run(() => editMetadata(row)); actions.append(edit);
      } else {
        const rename = document.createElement('button'); rename.textContent = text.rename;
        rename.setAttribute('aria-label', text.rename + ': ' + name);
        rename.onclick = () => run(async () => {
          const answer = prompt(text.newName, name); if (answer === null) return;
          const value = answer.trim();
          if (!value || new TextEncoder().encode(value).length > 64 || /[\x00-\x1f\x7f]/.test(value)) throw new Error('invalid_name');
          await api('account/rename', {kind, id:row.id, name:value}); await refreshRecords(); say('saved');
        });
        actions.append(rename);
      }
      actions.append(remove); item.append(details, actions); $(target).append(item);
    });
    return images;
  }
  async function passkey(operation, phone = false) {
    if (!window.isSecureContext || !window.PublicKeyCredential || !navigator.credentials) { say('unsupported'); return; }
    const create = operation !== 'get';
    let name = '';
    if (operation === 'register') {
      const answer = prompt(text.accountName);
      if (answer === null) return;
      name = answer.trim();
      if (!name || new TextEncoder().encode(name).length > 64 || /[\x00-\x1f\x7f]/.test(name)) throw new Error('invalid_name');
    }
    const options = await api('passkey/options', {operation, name});
    // Hints prefer a standard browser-owned hybrid ceremony. Never generate
    // our own QR code or restrict discoverable login to fabricated credentials.
    if (phone) options.hints = ['hybrid'];
    else if (!create) options.hints = ['client-device'];
    options.challenge = decode(options.challenge);
    if (create) {
      options.user.id = decode(options.user.id);
      options.excludeCredentials.forEach(c => { c.id = decode(c.id); });
      if (phone) options.authenticatorSelection.authenticatorAttachment = 'cross-platform';
    }
    say(phone ? 'phoneWaiting' : 'passkeyWaiting');
    let credential;
    try {
      credential = await navigator.credentials[create ? 'create' : 'get']({publicKey:options});
    } catch (error) {
      if (phone && error.name === 'NotAllowedError') throw new Error('phoneCancelled');
      if (error.name === 'NotSupportedError') throw new Error('unsupported');
      if (error.name === 'InvalidStateError') throw new Error('passkeyExists');
      throw error;
    }
    if (!credential) throw new Error(phone ? 'phoneCancelled' : 'cancelled');
    const response = credential.response;
    const result = {operation,id:encode(credential.rawId),clientDataJSON:encode(response.clientDataJSON)};
    if (create) result.attestationObject = encode(response.attestationObject);
    else Object.assign(result,{authenticatorData:encode(response.authenticatorData),signature:encode(response.signature),userHandle:response.userHandle ? encode(response.userHandle) : ''});
    await api('passkey/finish', result); await refresh();
    if (create) say(operation === 'register' ? 'accountCreated' : 'added');
  }
  async function start() {
    const translations = await (await fetch('/auth/i18n.json', {cache:'no-store'})).json();
    const supported = Object.keys(translations);
    const preferences = navigator.languages || [navigator.language];
    let language = 'en';
    for (const preference of preferences) {
      const tag = preference.toLowerCase();
      const match = supported.find(l => l.toLowerCase() === tag) ||
        (tag.startsWith('zh') ? (tag.includes('hk') ? 'zh-HK' : tag.includes('hant') || tag.includes('tw') ? 'zh-TW' : 'zh-CN') :
         tag.startsWith('pt') ? 'pt-PT' : supported.find(l => l === tag.split('-')[0]));
      if (match) { language = match; break; }
    }
    text = translations[language]; document.documentElement.lang = language; document.documentElement.dir = language === 'ar' ? 'rtl' : 'ltr';
    for (const id of ['intro','google','passkey','phoneAction','phone','registerPhone','registerHint','add','addPhone','addhint','nextStepTitle','nativeRestartHint','approve','warning','devicesTitle','keysTitle']) $(id).textContent = text[id];
    for (const id of ['logout','restart','refresh']) $(id).textContent = text[id];
    action('google', async () => { const result = await api('google/start'); const url = new URL(result.url); if (url.origin !== 'https://accounts.google.com') throw new Error('error'); location.assign(url.href); });
    action('passkey', () => passkey('get')); action('phoneAction', () => passkey('get', true));
    action('registerPhone', () => passkey('register', true));
    action('add', () => passkey('create')); action('addPhone', () => passkey('create', true));
    action('approve', async () => { await api('device/approve'); await refresh(); });
    action('refresh', refreshRecords);
    for (const id of ['serverTitle','serverHint','diagnosticsTitle','diagnosticsHint','historyTitle','historyHint','nameLabel','groupLabel','tagsLabel','noteLabel','orderLabel','metadataHint','metadataCancel','metadataSave']) $(id).textContent = text[id];
    $('metadataName').title = text.newName;
    $('deviceSearch').placeholder = text.searchDevices; $('deviceSearch').setAttribute('aria-label', text.searchDevices);
    $('groupFilter').setAttribute('aria-label', text.groupLabel); $('deviceSearch').oninput = renderDevices; $('groupFilter').onchange = renderDevices;
    $('metadataCancel').onclick = () => $('metadataDialog').close();
    $('metadataForm').onsubmit = event => {
      event.preventDefault();
      run(async () => {
        const name = $('metadataName').value.trim();
        const group = $('metadataGroup').value.trim(), note = $('metadataNote').value.trim();
        const tags = [...new Set($('metadataTags').value.split(',').map(t => t.trim()).filter(Boolean))];
        const order = Number($('metadataOrder').value), bytes = value => new TextEncoder().encode(value).length;
        if (!name || bytes(name) > 64 || /[\x00-\x1f\x7f]/.test(name)) {
          $('metadataError').textContent = text.invalid_name; return;
        }
        if (bytes(group) > 64 || bytes(note) > 256 || tags.length > 8 || tags.some(t => bytes(t) > 32) ||
            [group,note,...tags].some(t => /[\x00-\x1f\x7f]/.test(t)) || !Number.isInteger(order) || order < 0 || order > 9999) {
          $('metadataError').textContent = text.metadataHint; return;
        }
        try { await api('account/metadata', {id:editingDevice,name,group,tags,note,order}); }
        catch (error) { $('metadataError').textContent = text[error.message] || text.error; return; }
        $('metadataDialog').close(); await refreshRecords(); say('saved');
      });
    };
    for (const id of ['serverPanel','historyPanel']) $(id).ontoggle = () => {
      if ($(id).open && signedIn && !busy) run(refreshPanels);
    };
    action('restart', async () => {
      if (enrollmentFlow) { $('nativeRestartHint').hidden = false; return; }
      const url = new URL(location.href); url.searchParams.delete('flow'); url.searchParams.delete('error');
      location.assign(url.href);
    });
    action('logout', async () => {
      await api('browser/logout'); clearAvatars(); recordsCache = null; $('devices').replaceChildren();
      signedIn = false; awaitingNative = false; csrf = ''; accountId = '';
      if (!enrollmentFlow) flow = null;
      $('account').hidden = true; $('login').hidden = true; $('device').hidden = true;
      $('restart').hidden = enrollmentFlow; $('nativeRestartHint').hidden = !enrollmentFlow; say('signedOut');
      const url = new URL(location.href);
      if (!enrollmentFlow) url.searchParams.delete('flow');
      url.searchParams.delete('error');
      history.replaceState(null, '', url);
    });
    const loginError = new URL(location.href).searchParams.get('error');
    await refresh();
    setInterval(() => {
      if (!awaitingNative || busy || document.hidden) return;
      refreshQuietly(async () => {
        const state = await api('browser/start', {flow});
        if (state.claimed) {
          awaitingNative = false; updateNextStep(state); say('registrationComplete'); await refreshRecords();
        }
      });
    }, 3000);
    if (loginError) say(text[loginError] ? loginError : 'error');
    setInterval(() => {
      if (signedIn && !busy && !document.hidden && !$('account').contains(document.activeElement)) refreshQuietly(management ? refresh : refreshRecords);
    }, 15000);
  }
  start().catch(failed);
})();
