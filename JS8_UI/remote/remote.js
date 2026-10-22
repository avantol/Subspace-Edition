// [phone remote, operator 2026-10-21] Subspace Remote -- the phone page.
// It speaks the station's own JSON API: POST /api with one message, its
// reply comes back; GET /events streams every broadcast. Phase 1 shows
// what the station's screen shows (band activity, conversation, status);
// it does not control the station yet.
'use strict';

const $ = (id) => document.getElementById(id);
let nextId = Date.now() - 1499299200000; // the app's own id epoch
let events = null;

// [operator 2026-10-21, Android] The phone's browser did not send the
// login cookie back, so the session key from the login reply goes with
// every request itself (kept for this tab, so a reload stays logged in).
let session = '';
try { session = sessionStorage.getItem('js8session') || ''; } catch (e) { /* none */ }
function setSession(s) {
  session = s || '';
  try { sessionStorage.setItem('js8session', session); } catch (e) { /* none */ }
}

// [operator 2026-10-21: "I still have to enter the pass code"] The session
// ends whenever the station restarts (or another tab logs in), so the page
// keeps the pass code on the phone and logs in again by itself. Only a
// code that no longer works (File > Phone remote > New code) shows the
// login screen -- and is forgotten.
const CODE_KEY = 'js8passcode';
const savedCode = () => { try { return localStorage.getItem(CODE_KEY) || ''; } catch (e) { return ''; } };
const saveCode = (c) => { try { c ? localStorage.setItem(CODE_KEY, c) : localStorage.removeItem(CODE_KEY); } catch (e) { /* none */ } };

async function relogin() {
  const code = savedCode();
  if (!code) return false;
  if (await login(code)) return true;
  saveCode('');
  return false;
}

async function api(type, value = '', params = {}, retried = false) {
  params._ID = ++nextId;
  const r = await fetch('/api', {
    method: 'POST', credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json', 'X-Session': session },
    body: JSON.stringify({ type, value, params }),
  });
  if (r.status === 401) {
    if (!retried && await relogin()) {
      reopenEvents();
      return api(type, value, params, true);
    }
    showLogin();
    throw new Error('login');
  }
  return r.json();
}

async function login(code) {
  const r = await fetch('/login', {
    method: 'POST', credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ passcode: code }),
  });
  if (r.ok) {
    try { setSession((await r.json()).session); } catch (e) { /* cookie only */ }
    saveCode(code);
    return true;
  }
  $('loginError').textContent = r.status === 429
    ? 'Too many wrong codes. Wait a minute and try again.'
    : 'Wrong pass code.';
  return false;
}

function showLogin() {
  if (events) { events.close(); events = null; }
  $('app').classList.add('hidden');
  $('login').classList.remove('hidden');
}

function showApp() {
  $('login').classList.add('hidden');
  $('app').classList.remove('hidden');
}

const esc = (s) => String(s ?? '').replace(/[&<>"]/g,
  (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

function renderState(s) {
  if (!s) return;
  // [operator 2026-10-21] three decimals (kHz): six made the top bar wrap
  $('dial').textContent = (s.DIAL ? (s.DIAL / 1e6).toFixed(3) : '--') + ' ▾';
  $('offset').textContent = `${s.OFFSET} Hz`;
  stationMode = s.MODE;
  // [operator 2026-10-21/22] JTTY: an offset below 1200 or above 1800 Hz is
  // shown yellow -- outside the window WSJT-X searches for a signal's lowest
  // tone (its 1350/1650 +-150 Hz channels, jtty_receiver.cpp channelWindow;
  // the offset IS the lowest tone, so 1800 itself still decodes)
  $('offset').classList.toggle('warn', s.MODE === 'JTTY' && (s.OFFSET < 1200 || s.OFFSET > 1800));
  // in JS8 mode the speed is a tap target (the Mode menu's speeds)
  $('mode').textContent = s.MODE === 'JTTY' ? 'JTTY' : `JS8 ${s.SPEED || ''} ▾`;
  $('mode').classList.toggle('tappable', s.MODE !== 'JTTY');
  $('selected').textContent = s.SELECTED || '------';
  $('txen').textContent = s.TX_ENABLED ? 'TX on' : 'TX off';
  $('txen').classList.toggle('on', !!s.TX_ENABLED);
  $('txing').classList.toggle('hidden', !s.TRANSMITTING);
  const send = document.querySelector('[data-action="send"]');
  send.classList.toggle('busy', !!s.TRANSMITTING || !!s.QUEUED);
  send.textContent = s.TRANSMITTING ? 'Sending' : s.QUEUED ? 'Queued' : 'Send';
  // each phone button follows its desktop button: disabled there, here too
  const b = s.BUTTONS || {};
  document.querySelectorAll('#compose [data-action]').forEach((el) => {
    el.disabled = !b[el.dataset.action];
  });
  $('directed').classList.toggle('hidden', s.MODE !== 'JTTY'); // a menu in JS8
  $('directed').classList.toggle('latched', !!s.DIRECTED_LATCH);
  $('directed').textContent = s.SELECTED ? `Directed to ${s.SELECTED}` : 'Directed to';
  if (s.TU73_TEXT) $('tu73').textContent = s.TU73_TEXT; // TYPING in JS8 mode
  // [operator 2026-10-21] SNR? and Group are JS8-mode buttons
  document.querySelectorAll('.js8only').forEach((el) => el.classList.toggle('hidden', s.MODE === 'JTTY'));
  $('groupBtn').disabled = !!s.TRANSMITTING || !!s.QUEUED;
  $('groupBtn').classList.toggle('latched', !!(s.SELECTED && s.SELECTED.startsWith('@')));
  // the desktop's outgoing box, unless the phone is typing in its own
  const box = $('outgoing');
  // [operator 2026-10-21] locked from Send until the message has gone out
  // (no type-ahead or corrections from the phone yet)
  box.readOnly = !!(s.TRANSMITTING || s.QUEUED);
  box.classList.toggle('locked', box.readOnly);
  if (document.activeElement !== box && box.value !== (s.OUTGOING || '')) box.value = s.OUTGOING || '';
}

let bandRows = [];
let stationMode = 'JS8';
function renderBand(rows) {
  if (!rows) return;
  bandRows = rows;
  // [operator 2026-10-21] in JTTY mode every line is JTTY: the speed is
  // left off the first line, it is assumed
  const showSpeed = stationMode !== 'JTTY';
  $('band').innerHTML = rows.map((r, i) => `
    <div class="brow${r.TOME ? ' tome' : ''}${r.SELECTED ? ' selected' : ''}" data-i="${i}">
      <div class="top"><span class="call">${esc(r.CALL || r.OFFSET + ' Hz')}</span>
        <span>${esc(r.OFFSET)} Hz</span><span>${esc(r.SNR)}</span>
        ${showSpeed ? `<span>${esc(r.SPEED)}</span>` : ''}<span>${esc(r.AGE)}</span></div>
      <div class="msg">${esc(r.TEXT)}</div>
    </div>`).join('');
}

// [phone remote phase 2] tap = the desktop's click on that row, double-tap
// = its double-click (select + QSY), both through the station's handlers
let tapTimer = null, tapRow = null;
$('band').addEventListener('click', (e) => {
  const el = e.target.closest('.brow');
  if (!el) return;
  const r = bandRows[+el.dataset.i];
  if (!r) return;
  if (!r.CALL) { notice('Pick the station for that line on the desktop.'); return; }
  if (tapTimer && tapRow && tapRow.CALL === r.CALL) {
    clearTimeout(tapTimer); tapTimer = null;
    bandClick(r, true);
    return;
  }
  tapRow = r;
  tapTimer = setTimeout(() => { tapTimer = null; bandClick(r, false); }, 350);
});

async function bandClick(r, dbl) {
  notice(dbl ? `Selecting ${r.CALL} and moving to ${r.OFFSET} Hz...` : `Selecting ${r.CALL}...`);
  try {
    const rep = await api('UI.BAND_CLICK', '', { CALL: r.CALL, OFFSET: r.OFFSET, DOUBLE: dbl });
    if (rep.params && rep.params.OK === false) notice('That line has moved; try again.');
    else if (rep.params && rep.params.NO_QSY === 'hb_zone')
      notice(`${r.CALL} selected. Unable to QSY to HB zone (${r.OFFSET} Hz).`);
    await refresh();
  } catch (e) { /* login shown */ }
}

// [operator 2026-10-21: "it selects ok, i just don't have any feedback"]
// After every tap or button the phone reads the screen back itself, so
// the result shows even if the event stream is slow or reconnecting.
async function refresh() {
  const p = (await api('UI.GET_SNAPSHOT')).params || {};
  renderState(p.STATE); renderBand(p.ROWS); renderConvo(p.LINES);
}

let noticeTimer = null;
function notice(text) {
  const t = $('notice');
  t.textContent = text;
  t.classList.add('show');
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => t.classList.remove('show'), 3500);
}

// typing goes to the desktop's outgoing box (after a short pause)
let typeTimer = null;
async function pushText() {
  clearTimeout(typeTimer); typeTimer = null;
  try { await api('TX.SET_TEXT', $('outgoing').value.toUpperCase()); } catch (e) { /* login shown */ }
}
$('outgoing').addEventListener('input', () => {
  if ($('outgoing').readOnly) return;
  clearTimeout(typeTimer);
  typeTimer = setTimeout(pushText, 500);
});

const refusals = {
  disabled: 'Not available right now (it is greyed out on the station too).',
  js8_menu: 'In JS8 mode that button opens a menu on the station.',
  no_call: 'Select a station first.',
  queued: 'Already queued: it goes out at the next slot. HALT cancels it.',
  no_payload: 'Select or type a message to send',
};
document.querySelectorAll('[data-action]').forEach((el) => el.addEventListener('click', async () => {
  const action = el.dataset.action;
  try {
    if (action === 'send' && typeTimer) await pushText(); // the box first
    const rep = await api('UI.ACTION', action);
    const p = rep.params || {};
    if (p.OK === false) notice(refusals[p.REASON] || `Refused (${p.REASON}).`);
    if (p.OK && p.LOGGED) notice(`Logged ${p.LOGGED}.`);
    if (action === 'send' && p.OK) {
      // lock at once; the station's state keeps it locked until sent
      clearTimeout(typeTimer); typeTimer = null;
      $('outgoing').readOnly = true;
      $('outgoing').classList.add('locked');
      $('outgoing').blur();
    }
    await refresh();
  } catch (e) { /* login shown */ }
}));

let convoLines = [];
function renderConvo(lines) {
  if (!lines) return;
  convoLines = lines;
  const box = $('convo');
  const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 8;
  box.innerHTML = lines.map((l, i) =>
    `<div class="line${l.TX ? ' tx' : ''}" data-i="${i}">${esc(l.TEXT)}</div>`).join('');
  if (atBottom) box.scrollTop = box.scrollHeight; // follow, unless reading back
}

// [operator 2026-10-21] a conversation line: tap selects its first
// "CALL:" sign, double-tap also moves to the line's offset
let convoTimer = null, convoTapI = -1;
$('convo').addEventListener('click', (e) => {
  const el = e.target.closest('.line');
  if (!el) return;
  const i = +el.dataset.i;
  const l = convoLines[i];
  if (!l) return;
  if (convoTimer && convoTapI === i) {
    clearTimeout(convoTimer); convoTimer = null;
    convoClick(l, true);
    return;
  }
  convoTapI = i;
  convoTimer = setTimeout(() => { convoTimer = null; convoClick(l, false); }, 350);
});

async function convoClick(l, dbl) {
  try {
    const p = (await api('UI.CONVO_CLICK', l.TEXT, { DOUBLE: dbl })).params || {};
    if (p.REASON === 'no_call') notice('No \'from\' call sign detected');
    else if (p.NO_QSY === 'hb_zone') notice(`${p.CALL} selected. Unable to QSY to HB zone (${p.OFFSET} Hz).`);
    else if (p.OK) notice(dbl && p.OFFSET > 0 ? `${p.CALL} selected, moved to ${p.OFFSET} Hz.` : `${p.CALL} selected.`);
    await refresh();
  } catch (e) { /* login shown */ }
}

function setConn(ok) {
  $('conn').classList.toggle('on', ok);
  $('conn').classList.toggle('off', !ok);
}

async function start() {
  const snap = await api('UI.GET_SNAPSHOT');
  const p = snap.params || {};
  showApp();
  renderState(p.STATE); renderBand(p.ROWS); renderConvo(p.LINES);
  reopenEvents();
}

// The event stream, with the current session key (a new one after every
// login, so it is reopened then). While the station is down the browser
// keeps retrying by itself; when it is back, the old key is refused, the
// stream closes, and start() logs in again and reopens it.
function reopenEvents() {
  if (events) events.close();
  events = new EventSource('/events?s=' + encodeURIComponent(session));
  events.onopen = () => setConn(true);
  events.onerror = async () => {
    setConn(false);
    if (events && events.readyState === EventSource.CLOSED) {
      try { await start(); } catch (e) { /* api() showed the login */ }
    }
  };
  events.onmessage = (ev) => {
    let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
    const p = m.params || {};
    if (m.type === 'RX.TEXT.TAIL') renderConvo(p.LINES);
    else if (m.type === 'RX.BAND.TABLE') renderBand(p.ROWS);
    else if (m.type === 'UI.STATE') renderState(p.STATE);
  };
}

// [operator 2026-10-21] -/+ beside the offset: 25 Hz a tap, through the
// station's own offset change (refused there while transmitting)
document.querySelectorAll('button.step').forEach((b) => b.addEventListener('click', async () => {
  try {
    const p = (await api('UI.OFFSET_STEP', '', { STEP: +b.dataset.step })).params || {};
    if (p.REASON === 'hb_zone') notice('Unable to QSY to HB zone (1000 Hz and below).');
    else if (p.OK === false) notice('The offset cannot change while transmitting.');
    else if (p.OFFSET !== undefined) {
      $('offset').textContent = `${p.OFFSET} Hz`;
      $('offset').classList.toggle('warn', stationMode === 'JTTY' && (p.OFFSET < 1200 || p.OFFSET > 1800));
    }
  } catch (e) { /* login shown */ }
}));

function showTab(name) {
  document.querySelectorAll('#tabs button').forEach((x) => x.classList.toggle('active', x.dataset.tab === name));
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('hidden', t.id !== name));
  // [operator 2026-10-21] opening the Conversation lands on its newest line
  if (name === 'convo') requestAnimationFrame(() => { $('convo').scrollTop = $('convo').scrollHeight; });
}
let lastTab = 'band';
document.querySelectorAll('#tabs button').forEach((b) => b.addEventListener('click', () => {
  lastTab = b.dataset.tab;
  showTab(lastTab);
}));

// [operator 2026-10-21] JS8 mode: Group lists the call list's groups
// (@ALLCALL and My Groups); choosing one selects it as the station would
let groupItems = [];
$('groupBtn').addEventListener('click', async () => {
  try {
    groupItems = ((await api('UI.GET_GROUPS')).params || {}).ITEMS || [];
  } catch (e) { return; }
  // [operator 2026-10-21] "--None--" first: clears the selection (Deselect)
  $('groups').innerHTML = '<div class="frow" data-i="-2"><span class="mhz">--None--</span></div>' +
    (groupItems.map((g, i) => `
    <div class="frow${g.SELECTED ? ' current' : ''}" data-i="${i}">
      <span class="mhz">${esc(g.NAME)}</span></div>`).join('') ||
    '<p class="hint">No groups. Add them in the station\'s settings (My Groups).</p>') +
    '<div class="frow" data-i="-1"><span class="desc">Cancel</span></div>';
  showTab('groups');
});
$('groups').addEventListener('click', async (e) => {
  const el = e.target.closest('.frow');
  if (!el) return;
  if (el.dataset.i === '-2') {
    try {
      const p = (await api('UI.ACTION', 'deselect')).params || {};
      if (p.OK === false) notice('Not available right now.');
      await refresh();
    } catch (e2) { /* login shown */ }
    showTab('compose');
    return;
  }
  const g = groupItems[+el.dataset.i];
  if (!g || g.SELECTED) { showTab('compose'); return; }
  try {
    const p = (await api('UI.SELECT_GROUP', g.NAME)).params || {};
    if (p.OK === false) notice(`Refused (${p.REASON}).`);
    else notice(`${g.NAME} selected.`);
    await refresh();
  } catch (e2) { /* login shown */ }
  showTab('compose');
});

// [operator 2026-10-21] JS8 mode: the speed opens the Mode menu's speeds
// (its own wording); tapping it again closes the list
let speedItems = [];
async function loadSpeeds() {
  try {
    speedItems = ((await api('UI.GET_SPEEDS')).params || {}).ITEMS || [];
  } catch (e) { return; }
  $('speeds').innerHTML = speedItems.map((s, i) => `
    <div class="frow${s.CHECKED ? ' current' : ''}${s.ENABLED ? '' : ' off'}" data-i="${i}">
      <span class="mhz">${esc(s.TEXT)}</span>
    </div>`).join('');
}
$('mode').addEventListener('click', () => {
  if (stationMode === 'JTTY') return;
  if (!$('speeds').classList.contains('hidden')) { showTab(lastTab); return; }
  showTab('speeds');
  loadSpeeds();
});
$('speeds').addEventListener('click', async (e) => {
  const el = e.target.closest('.frow');
  if (!el) return;
  const s = speedItems[+el.dataset.i];
  if (!s) return;
  if (s.CHECKED) { showTab(lastTab); return; }
  if (!s.ENABLED) { notice('Not available right now (greyed out on the station too).'); return; }
  try {
    const p = (await api('UI.SET_SPEED', s.ID)).params || {};
    if (p.OK === false) notice(`Refused (${p.REASON}).`);
    await refresh();
    showTab(lastTab);
  } catch (e2) { /* login shown */ }
});

// [operator 2026-10-21: "tapping on the freq value should be all that's
// needed"] the dial opens the frequency list; tapping it again closes it
$('dial').addEventListener('click', () => {
  if (!$('freq').classList.contains('hidden')) { showTab(lastTab); return; }
  showTab('freq');
  loadFreqs();
});

// [operator 2026-10-21] The station's own frequency list (the Frequency
// menu's entries); a tap moves there through the menu's own action.
let freqItems = [];
async function loadFreqs() {
  try {
    freqItems = ((await api('UI.GET_FREQ_LIST')).params || {}).ITEMS || [];
  } catch (e) { return; }
  $('freq').innerHTML = freqItems.map((f, i) => `
    <div class="frow${f.CURRENT ? ' current' : ''}" data-i="${i}">
      <span class="band">${esc(f.BAND)}</span><span class="mhz">${esc(f.MHZ)}</span>
      <span class="kind">${esc(f.MODE)}</span><span class="desc">${esc(f.DESC)}</span>
    </div>`).join('') || '<p class="hint">The frequency list is empty.</p>';
}
// [operator 2026-10-21] a JS8+JTTY entry asks which mode: two buttons on
// the line itself (also on the current frequency, to switch mode there)
$('freq').addEventListener('click', async (e) => {
  const pick = e.target.closest('[data-mode]');
  if (pick) { e.stopPropagation(); return goFreq(freqItems[+pick.dataset.i], pick.dataset.mode); }
  const el = e.target.closest('.frow');
  if (!el) return;
  const i = +el.dataset.i;
  const f = freqItems[i];
  if (!f) return;
  if (f.SHARED) {
    document.querySelectorAll('#freq .choose').forEach((c) => c.remove());
    el.insertAdjacentHTML('afterend', `<div class="choose">
      <span>Mode on ${esc(f.MHZ)}:</span>
      <button data-i="${i}" data-mode="JS8" class="${stationMode !== 'JTTY' && f.CURRENT ? 'on' : ''}">JS8</button>
      <button data-i="${i}" data-mode="JTTY" class="${stationMode === 'JTTY' && f.CURRENT ? 'on' : ''}">JTTY</button></div>`);
    return;
  }
  // [operator 2026-10-21] the current entry may be chosen too (re-tunes the rig)
  if (!confirm(f.CURRENT ? `Set the radio to ${f.MHZ} MHz again?`
                         : `Change to ${f.MHZ} MHz (${f.BAND}, ${f.MODE})?`)) return;
  goFreq(f, '');
});

async function goFreq(f, mode) {
  if (!f) return;
  notice(f.CURRENT && mode ? `Switching to ${mode} mode...`
         : f.CURRENT        ? `Setting the radio to ${f.MHZ} MHz...`
                            : `Changing to ${f.MHZ} MHz${mode ? ', ' + mode + ' mode' : ''}...`);
  try {
    const p = (await api('UI.SET_FREQ_ENTRY', '', { FREQ: f.FREQ, MODE: mode })).params || {};
    if (p.OK === false)
      notice(p.REASON === 'transmitting' ? 'Not while transmitting.' : `Refused (${p.REASON}).`);
    await refresh();
    if (p.OK) showTab(lastTab); else await loadFreqs();
  } catch (e2) { /* login shown */ }
}

$('loginForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  if (await login($('passcode').value.trim())) start().catch(() => {});
});

(async () => {
  // the scanned address carries the pass code after '#'
  const code = decodeURIComponent(location.hash.slice(1));
  if (code) {
    history.replaceState(null, '', location.pathname); // off the screen
    if (!(await login(code))) { showLogin(); return; }
  }
  try { await start(); } catch (e) { showLogin(); }
})();
