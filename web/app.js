'use strict';
let csrf = '', st = null, tab = 'meds';
const $ = (s, r = document) => r.querySelector(s);
const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const app = () => $('#app');
const TYPES = {fixed:'Daily at times', routine:'Daily by routine', weekdays:'Weekdays', every_n:'Every N days/weeks', dates:'Specific dates', cycle:'Cycle on/off', prn:'As needed (PRN)'};
const FORMS = ['pill','capsule','liquid','injection','inhaler','drops','patch','cream','other'];
const ROUT = ['wake','breakfast','lunch','dinner','bed'];
const DAYS = ['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];

async function api(method, path, body, raw, extra) {
  const h = Object.assign({}, extra || {});
  if (method !== 'GET') h['X-CSRF-Token'] = csrf;
  if (body !== undefined && !raw) h['Content-Type'] = 'application/json';
  const r = await fetch(path, {method, headers: h, body: body === undefined ? undefined : raw ? body : JSON.stringify(body), credentials: 'same-origin'});
  let j = {};
  try { j = await r.json(); } catch (e) { j = {}; }
  if (r.status === 401) { csrf = ''; login('Session ended. Enter the new code shown on the device.'); throw new Error('login'); }
  if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
  return j;
}
function fail(e, el) { if (e.message !== 'login') (el || $('#msg')).textContent = e.message; }

function login(msg) {
  app().innerHTML = `<main><div class="card"><h2>Sign in</h2>
  <p>On the device open <b>Settings → Web portal</b> and type the one-time code shown there.</p>
  <label>Code</label><input id="code" inputmode="numeric" autocomplete="one-time-code" maxlength="12">
  <p class="err" id="msg">${esc(msg || '')}</p><button id="go">Sign in</button></div>
  <p class="warn">No TLS: this page is not encrypted. Use it only on your own home Wi-Fi.</p></main>`;
  const go = async () => {
    try {
      const r = await fetch('/api/login', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({code: $('#code').value.trim()})});
      const j = await r.json();
      if (!r.ok) throw new Error(j.error || 'Sign-in failed');
      csrf = j.csrf; await load();
    } catch (e) { $('#msg').textContent = e.message; }
  };
  $('#go').onclick = go; $('#code').onkeydown = e => { if (e.key === 'Enter') go(); };
  $('#code').focus();
}

async function load() { st = await api('GET', '/api/state'); $('#hd').textContent = st.today + ' ' + st.now; render(); }

function render() {
  const tabs = {meds: 'Medications', people: 'People', history: 'History', data: 'Data', more: 'More'};
  app().innerHTML = `<nav>${Object.keys(tabs).map(k => `<button data-t="${k}" class="${k === tab ? 'on' : ''}">${tabs[k]}</button>`).join('')}</nav><main id="m"></main>`;
  app().querySelectorAll('nav button').forEach(b => b.onclick = () => { tab = b.dataset.t; render(); });
  ({meds, people, history, data, more})[tab]();
}
const person = id => (st.profiles.find(p => p.id === id) || {name: '?'}).name;
const main = () => $('#m');

function meds() {
  const act = st.meds.filter(m => m.active !== false), arc = st.meds.filter(m => m.active === false);
  const card = m => `<div class="card"><div class="row"><div><b>${esc(m.name)}</b> ${esc(m.dose || '')}<div class="muted">${esc(person(m.profile))} · ${esc(m.phases.map(p => TYPES[p.rule.type]).join(' → '))}</div></div>
    <button class="alt" data-e="${m.id}">Edit</button></div></div>`;
  main().innerHTML = `<div class="btns" style="margin:0 0 12px"><button id="add">Add medication</button></div><p class="err" id="msg"></p>
    ${act.map(card).join('') || '<p class="muted">No medications yet.</p>'}
    ${arc.length ? '<h3>Archived</h3>' + arc.map(card).join('') : ''}`;
  $('#add').onclick = () => st.profiles.length ? medForm(null) : ($('#msg').textContent = 'Add a person first.');
  main().querySelectorAll('[data-e]').forEach(b => b.onclick = () => medForm(st.meds.find(m => m.id === +b.dataset.e)));
}

function ruleHtml(r, i) {
  const t = r.type || 'fixed', times = (r.times || []);
  const clock = times.filter(x => x.at).map(x => x.at).join(', ');
  const rt = times.filter(x => x.routine).map(x => x.routine + (x.offset ? (x.offset > 0 ? '+' : '') + x.offset : '')).join(', ');
  const wd = r.weekdays || 0;
  return `<label>Schedule type</label><select data-k="type">${Object.keys(TYPES).map(k => `<option value="${k}" ${k === t ? 'selected' : ''}>${TYPES[k]}</option>`).join('')}</select>
  <div data-s="times" ${t === 'prn' || t === 'routine' ? 'hidden' : ''}><label>Times (24 h, comma separated)</label><input data-k="clock" value="${esc(clock)}" placeholder="08:00, 20:00"></div>
  <div data-s="rt" ${t === 'routine' ? '' : 'hidden'}><label>Routines (${ROUT.join(', ')}; optional ±minutes)</label><input data-k="rt" value="${esc(rt)}" placeholder="breakfast, bed-30"></div>
  <div data-s="wd" class="wd" ${t === 'weekdays' ? '' : 'hidden'}>${DAYS.map((d, k) => `<label><input type="checkbox" data-wd="${k}" ${wd & (1 << k) ? 'checked' : ''}> ${d}</label>`).join('')}</div>
  <div data-s="ev" ${t === 'every_n' ? '' : 'hidden'} class="row"><div><label>Every</label><input data-k="every" type="number" min="1" max="365" value="${r.every || 2}"></div>
    <div><label>Unit</label><select data-k="unit"><option ${r.unit !== 'weeks' ? 'selected' : ''}>days</option><option ${r.unit === 'weeks' ? 'selected' : ''}>weeks</option></select></div></div>
  <div data-s="dt" ${t === 'dates' ? '' : 'hidden'}><label>Dates (YYYY-MM-DD, comma separated)</label><input data-k="dates" value="${esc((r.dates || []).join(', '))}"></div>
  <div data-s="cy" ${t === 'cycle' ? '' : 'hidden'} class="row"><div><label>Days on</label><input data-k="on" type="number" min="1" value="${r.on || 21}"></div><div><label>Days off</label><input data-k="off" type="number" min="1" value="${r.off || 7}"></div></div>
  <div data-s="prn" ${t === 'prn' ? '' : 'hidden'} class="row"><div><label>Min. minutes between</label><input data-k="minIntervalMin" type="number" min="1" value="${r.minIntervalMin || 240}"></div><div><label>Max per 24 h</label><input data-k="maxPer24h" type="number" min="1" max="24" value="${r.maxPer24h || 4}"></div></div>`;
}
function phaseHtml(p, i, n) {
  return `<div class="ph" data-ph="${i}"><div class="row"><b>Phase ${i + 1}</b>${n > 1 ? `<button class="del" data-rm="${i}">Remove</button>` : ''}</div>
  <div class="row"><div><label>From (optional)</label><input type="date" data-k="from" value="${esc(p.from || '')}"></div><div><label>To (optional)</label><input type="date" data-k="to" value="${esc(p.to || '')}"></div></div>
  <label>Dose in this phase (optional)</label><input data-k="dose" value="${esc(p.dose || '')}">${ruleHtml(p.rule || {}, i)}</div>`;
}
function readRule(el) {
  const v = k => $(`[data-k="${k}"]`, el).value.trim(), t = v('type'), r = {type: t};
  const list = s => s.split(',').map(x => x.trim()).filter(Boolean);
  if (t === 'routine') r.times = list(v('rt')).map(x => { const m = /^([a-z]+)\s*([+-]\d+)?$/.exec(x); if (!m) throw new Error('Routine "' + x + '" not understood'); return m[2] ? {routine: m[1], offset: +m[2]} : {routine: m[1]}; });
  else if (t !== 'prn') r.times = list(v('clock')).map(x => ({at: x}));
  if (t === 'weekdays') { r.weekdays = 0; el.querySelectorAll('[data-wd]').forEach(c => { if (c.checked) r.weekdays |= 1 << +c.dataset.wd; }); if (!r.weekdays) throw new Error('Pick at least one weekday'); }
  if (t === 'every_n') { r.every = +v('every'); r.unit = v('unit'); }
  if (t === 'dates') r.dates = list(v('dates'));
  if (t === 'cycle') { r.on = +v('on'); r.off = +v('off'); }
  if (t === 'prn') { r.minIntervalMin = +v('minIntervalMin'); r.maxPer24h = +v('maxPer24h'); }
  return r;
}
function readPhases() {
  return [...main().querySelectorAll('[data-ph]')].map(el => {
    const p = {rule: readRule(el)}, g = k => $(`[data-k="${k}"]`, el).value.trim();
    if (g('from')) p.from = g('from'); if (g('to')) p.to = g('to'); if (g('dose')) p.dose = g('dose');
    return p;
  });
}
function medForm(m) {
  const isNew = !m;
  m = m || {profile: st.profiles[0].id, name: '', dose: '', form: 'pill', start: st.today, phases: [{rule: {type: 'fixed', times: [{at: '08:00'}]}}], active: true};
  let phases = JSON.parse(JSON.stringify(m.phases));
  const draw = () => {
    main().innerHTML = `<div class="card"><h2>${isNew ? 'Add medication' : 'Edit ' + esc(m.name)}</h2>
    <label>Person</label><select id="f-profile">${st.profiles.map(p => `<option value="${p.id}" ${p.id === m.profile ? 'selected' : ''}>${esc(p.name)}</option>`).join('')}</select>
    <label>Name</label><input id="f-name" value="${esc(m.name)}" maxlength="40">
    <div class="row"><div><label>Dose</label><input id="f-dose" value="${esc(m.dose || '')}" placeholder="1 tablet"></div>
    <div><label>Form</label><select id="f-form">${FORMS.map(f => `<option ${f === m.form ? 'selected' : ''}>${f}</option>`).join('')}</select></div></div>
    <label>Instructions</label><input id="f-instructions" value="${esc(m.instructions || '')}" placeholder="with food">
    <div class="row"><div><label>Start</label><input type="date" id="f-start" value="${esc(m.start || st.today)}"></div><div><label>End (optional)</label><input type="date" id="f-end" value="${esc(m.end || '')}"></div></div>
    <div class="row"><div><label>Supply (−1 = not tracked)</label><input type="number" id="f-supply" value="${m.supply == null ? -1 : m.supply}"></div><div><label>Warn below</label><input type="number" id="f-lowStock" value="${m.lowStock || 0}"></div></div>
    <label><input type="checkbox" id="f-critical" ${m.critical ? 'checked' : ''}> Critical (louder, longer reminders)</label>
    <h3>Schedule</h3><div id="phs">${phases.map((p, i) => phaseHtml(p, i, phases.length)).join('')}</div>
    ${phases.length < 4 ? '<button class="alt" id="addph">Add phase (taper)</button>' : ''}
    <p class="err" id="msg"></p><div class="btns"><button id="save">Save</button><button class="alt" id="cancel">Cancel</button>
    ${isNew ? '' : `<button class="alt" id="arch">${m.active === false ? 'Restore' : 'Archive'}</button>`}
    ${!isNew && m.active === false ? '<button class="del" id="delm">Delete</button>' : ''}</div></div>`;
    main().querySelectorAll('[data-k="type"]').forEach(s => s.onchange = () => { try { phases = readPhases(); } catch (e) { phases[+s.closest('[data-ph]').dataset.ph].rule = {type: s.value}; } draw(); });
    main().querySelectorAll('[data-rm]').forEach(b => b.onclick = () => { try { phases = readPhases(); } catch (e) {} phases.splice(+b.dataset.rm, 1); draw(); });
    if ($('#addph')) $('#addph').onclick = () => { try { phases = readPhases(); } catch (e) {} phases.push({rule: {type: 'fixed', times: [{at: '08:00'}]}}); draw(); };
    $('#cancel').onclick = () => render();
    $('#save').onclick = async () => {
      try {
        const b = Object.assign({}, m, {profile: +$('#f-profile').value, name: $('#f-name').value.trim(), dose: $('#f-dose').value.trim(), form: $('#f-form').value,
          instructions: $('#f-instructions').value.trim(), start: $('#f-start').value, supply: +$('#f-supply').value, lowStock: +$('#f-lowStock').value,
          critical: $('#f-critical').checked, phases: readPhases()});
        if ($('#f-end').value) b.end = $('#f-end').value; else delete b.end;
        delete b.id;
        await api(isNew ? 'POST' : 'PUT', isNew ? '/api/meds' : '/api/meds/' + m.id, b);
        await load();
      } catch (e) { fail(e); }
    };
    if ($('#arch')) $('#arch').onclick = async () => { try { await api('POST', `/api/meds/${m.id}/archive`, {active: m.active === false}); await load(); } catch (e) { fail(e); } };
    if ($('#delm')) $('#delm').onclick = async () => { if (!confirm('Delete ' + m.name + '? History entries stay.')) return; try { await api('DELETE', '/api/meds/' + m.id); await load(); } catch (e) { fail(e); } };
  };
  draw();
}

function people() {
  main().innerHTML = `${st.profiles.length < st.maxProfiles ? '<div class="btns" style="margin:0 0 12px"><button id="add">Add person</button></div>' : ''}<p class="err" id="msg"></p>
  ${st.profiles.map(p => `<div class="card row"><div><b>${esc(p.name)}</b><div class="muted">${p.hasPin ? 'PIN set on device' : 'No PIN'}${p.privacy ? ' · privacy on' : ''}</div></div><button class="alt" data-e="${p.id}">Edit</button></div>`).join('')}`;
  if ($('#add')) $('#add').onclick = () => personForm(null);
  main().querySelectorAll('[data-e]').forEach(b => b.onclick = () => personForm(st.profiles.find(p => p.id === +b.dataset.e)));
}
function personForm(p) {
  const isNew = !p; p = p || {name: '', routines: {}, quietStart: '22:00', quietEnd: '07:00'};
  const r = p.routines || {};
  main().innerHTML = `<div class="card"><h2>${isNew ? 'Add person' : 'Edit ' + esc(p.name)}</h2>
  <label>Name</label><input id="p-name" value="${esc(p.name)}" maxlength="40">
  <label><input type="checkbox" id="p-privacy" ${p.privacy ? 'checked' : ''}> Privacy (hide names on the home screen)</label>
  <h3>Routine times</h3><div class="row">${ROUT.map(k => `<div><label>${k}</label><input type="time" data-r="${k}" value="${esc(r[k] || '')}"></div>`).join('')}</div>
  <div class="row"><div><label>Quiet from</label><input type="time" id="p-qs" value="${esc(p.quietStart || '')}"></div><div><label>Quiet until</label><input type="time" id="p-qe" value="${esc(p.quietEnd || '')}"></div></div>
  <p class="muted">PINs can only be set on the device.</p><p class="err" id="msg"></p>
  <div class="btns"><button id="save">Save</button><button class="alt" id="cancel">Cancel</button>${isNew ? '' : '<button class="del" id="delp">Delete</button>'}</div></div>`;
  $('#cancel').onclick = () => render();
  $('#save').onclick = async () => {
    const b = {name: $('#p-name').value.trim(), privacy: $('#p-privacy').checked, routines: {}};
    main().querySelectorAll('[data-r]').forEach(i => { if (i.value) b.routines[i.dataset.r] = i.value; });
    if ($('#p-qs').value) b.quietStart = $('#p-qs').value; if ($('#p-qe').value) b.quietEnd = $('#p-qe').value;
    try { await api(isNew ? 'POST' : 'PUT', isNew ? '/api/profiles' : '/api/profiles/' + p.id, b); await load(); } catch (e) { fail(e); }
  };
  if ($('#delp')) $('#delp').onclick = async () => { if (!confirm('Delete ' + p.name + '?')) return; try { await api('DELETE', '/api/profiles/' + p.id); await load(); } catch (e) { fail(e); } };
}

async function history() {
  main().innerHTML = `<div class="card row"><select id="h-p"><option value="0">Everyone</option>${st.profiles.map(p => `<option value="${p.id}">${esc(p.name)}</option>`).join('')}</select>
  <select id="h-d"><option value="7">7 days</option><option value="14" selected>14 days</option><option value="30">30 days</option><option value="90">90 days</option></select></div><p class="err" id="msg"></p><div id="h-t"></div>`;
  const show = async () => {
    try {
      const j = await api('GET', `/api/history?profile=${$('#h-p').value}&days=${$('#h-d').value}`);
      $('#h-t').innerHTML = j.entries.length ? `<table><tr><th>Due</th><th>Who</th><th>Medication</th><th>Result</th></tr>${j.entries.map(e => `<tr><td>${esc(e.date)} ${esc(e.scheduled)}</td><td>${esc(e.person)}</td><td>${esc(e.med)}</td><td>${esc(e.action)}<div class="muted">${esc(e.at)}</div></td></tr>`).join('')}</table>` : '<p class="muted">Nothing recorded in this period.</p>';
    } catch (e) { fail(e); }
  };
  $('#h-p').onchange = show; $('#h-d').onchange = show; show();
}

function data() {
  main().innerHTML = `<div class="card"><h3>Export history</h3><label><input type="checkbox" id="x-n"> Include names of people and medications</label>
  <div class="btns"><button id="x-csv">Download CSV</button><button class="alt" id="x-json">Download JSON</button></div></div>
  <div class="card"><h3>Backup</h3><p class="muted">Everything except Wi-Fi credentials and PINs in plain text (PINs stay hashed).</p><button id="b-dl">Download backup</button></div>
  <div class="card"><h3>Restore</h3><p class="muted">Replaces all people, medications and settings on the device. The backup is checked fully before anything changes.</p>
  <input type="file" id="r-f" accept=".json,application/json"><div class="btns"><button class="del" id="r-go">Restore backup</button></div></div><p class="err" id="msg"></p>`;
  const dl = p => { location.href = p; };
  $('#x-csv').onclick = () => dl('/api/export.csv' + ($('#x-n').checked ? '?names=1' : ''));
  $('#x-json').onclick = () => dl('/api/export.json' + ($('#x-n').checked ? '?names=1' : ''));
  $('#b-dl').onclick = () => dl('/api/backup');
  $('#r-go').onclick = async () => {
    const f = $('#r-f').files[0]; if (!f) return ($('#msg').textContent = 'Choose a backup file first.');
    if (!confirm('Replace everything on the device with ' + f.name + '?')) return;
    try { const j = await api('POST', '/api/restore', await f.text(), true, {'Content-Type': 'application/json'}); $('#msg').textContent = j.message; await load(); } catch (e) { fail(e); }
  };
}

async function more() {
  main().innerHTML = `<div class="card"><h3>Caregiver alerts</h3><p class="muted">Sends a message when a dose is missed. ntfy.sh topic URL or any webhook (JSON POST).</p>
  <label><input type="checkbox" id="n-en"> Enabled</label><label>URL</label><input id="n-url" placeholder="https://ntfy.sh/your-secret-topic">
  <label><input type="checkbox" id="n-nm"> Include person and medication names</label><div class="btns"><button id="n-save">Save</button></div><p class="err" id="n-msg"></p></div>
  <div class="card"><h3>Firmware update</h3><p class="muted">Running ${esc(st.version)}. Choose firmware.bin and paste its SHA-256 (from the .sha256 file next to the release). The device checks both before switching and returns to the old version if the new one fails to start.</p>
  <label>Firmware file</label><input type="file" id="o-f" accept=".bin"><label>SHA-256 (64 hex characters) or .sha256 file</label><input id="o-sha" maxlength="200" placeholder="e3b0c442…">
  <input type="file" id="o-shf" accept=".sha256,.txt"><div class="btns"><button id="o-go">Upload and install</button></div><p class="err" id="o-msg"></p></div>
  <div class="card"><button class="alt" id="lo">Sign out</button></div><p class="warn">No TLS: traffic to the device is not encrypted.</p>`;
  try { const n = await api('GET', '/api/notify'); $('#n-en').checked = n.enabled; $('#n-url').value = n.url; $('#n-nm').checked = n.includeNames; } catch (e) { fail(e, $('#n-msg')); }
  $('#n-save').onclick = async () => { try { await api('PUT', '/api/notify', {enabled: $('#n-en').checked, url: $('#n-url').value.trim(), includeNames: $('#n-nm').checked}); $('#n-msg').textContent = 'Saved.'; } catch (e) { fail(e, $('#n-msg')); } };
  $('#o-shf').onchange = async () => { const f = $('#o-shf').files[0]; if (f) $('#o-sha').value = ((await f.text()).match(/[0-9a-fA-F]{64}/) || [''])[0]; };
  $('#o-go').onclick = async () => {
    const f = $('#o-f').files[0], sha = $('#o-sha').value.trim();
    if (!f || !/^[0-9a-fA-F]{64}$/.test(sha)) return ($('#o-msg').textContent = 'Choose the firmware file and enter its SHA-256.');
    $('#o-go').disabled = true; $('#o-msg').textContent = 'Uploading ' + Math.round(f.size / 1024) + ' KB…';
    try { const j = await api('POST', '/api/ota', f, true, {'Content-Type': 'application/octet-stream', 'X-Firmware-SHA256': sha}); $('#o-msg').textContent = j.message + ' (' + j.version + ')'; }
    catch (e) { fail(e, $('#o-msg')); $('#o-go').disabled = false; }
  };
  $('#lo').onclick = async () => { try { await api('POST', '/api/logout', {}); } catch (e) {} csrf = ''; login('Signed out.'); };
}

(async () => {
  try {
    const r = await fetch('/api/session', {credentials: 'same-origin'});
    if (r.ok) { csrf = (await r.json()).csrf; await load(); } else login();
  } catch (e) { login(); }
})();
