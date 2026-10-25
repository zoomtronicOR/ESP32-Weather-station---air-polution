// Meteo stanica - web interfejs (bez spoljnih biblioteka, radi i bez interneta)
'use strict';

const DAYS = ['nedelja', 'ponedeljak', 'utorak', 'sreda', 'četvrtak', 'petak', 'subota'];
const AQ_COLORS = ['', '#00C8FF', '#00FF40', '#FFD000', '#FF6000', '#FF0000', '#B000FF'];
const ICONS = {
  temperature: '🌡️', humidity: '💧', pressure: '⏲️', gas_resistance: '🧪',
  pm1_0: '🌫️', pm2_5: '🌫️', pm4: '🌫️', pm10: '🌫️', typical_particle_size: '⚫',
  uv_index: '☀️', illuminance: '💡', pitch: '📐', roll: '📐', motion: '🏃', air_quality_level: '🍃',
};

const state = {
  status: null,
  clockOffsetMs: null,  // vreme uređaja - vreme pregledača
  timezone: 'Europe/Belgrade',
  page: null,
  pageTimer: null,
};

// ---------------------------------------------------------------------------
// Pomoćne funkcije
// ---------------------------------------------------------------------------

const $ = (sel) => document.querySelector(sel);

function esc(v) {
  return String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function ago(seconds) {
  if (seconds == null) return '-';
  if (seconds < 60) return `pre ${seconds} s`;
  if (seconds < 3600) return `pre ${Math.floor(seconds / 60)} min`;
  if (seconds < 86400) return `pre ${Math.floor(seconds / 3600)} h`;
  return `pre ${Math.floor(seconds / 86400)} d`;
}

function duration(seconds) {
  const d = Math.floor(seconds / 86400), h = Math.floor(seconds / 3600) % 24, m = Math.floor(seconds / 60) % 60;
  return d > 0 ? `${d} d ${h} h ${m} min` : `${h} h ${m} min`;
}

function bytes(n) {
  if (n >= 1048576) return (n / 1048576).toFixed(2) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

let bannerTimer = null;
function banner(text, isError = false) {
  const b = $('#banner');
  b.textContent = text;
  b.className = 'banner' + (isError ? ' error' : '');
  b.hidden = false;
  clearTimeout(bannerTimer);
  bannerTimer = setTimeout(() => { b.hidden = true; }, 4000);
}

// ---------------------------------------------------------------------------
// API i prijava
// ---------------------------------------------------------------------------

function storedAuth() {
  try { return sessionStorage.getItem('auth'); } catch (e) { return null; }
}

function setStoredAuth(value) {
  try {
    if (value) sessionStorage.setItem('auth', value); else sessionStorage.removeItem('auth');
  } catch (e) { /* privatni režim pregledača */ }
  updateAuthButton();
}

async function api(path, options = {}) {
  const opts = { ...options, headers: { ...(options.headers || {}) } };
  if (opts.body && typeof opts.body !== 'string') {
    opts.body = JSON.stringify(opts.body);
    opts.headers['Content-Type'] = 'application/json';
  }
  const auth = storedAuth();
  if (auth) opts.headers['Authorization'] = 'Basic ' + auth;
  const res = await fetch('/api/v1' + path, opts);
  let data = null;
  try { data = await res.json(); } catch (e) { /* prazan odgovor */ }
  if (!res.ok) {
    const err = new Error((data && data.error) || `Greška ${res.status}`);
    err.status = res.status;
    throw err;
  }
  return data;
}

// Akcija koja traži prijavu; po potrebi otvara prozor za lozinku i ponavlja zahtev
async function adminApi(path, body) {
  for (let attempt = 0; attempt < 2; attempt++) {
    if (!storedAuth()) {
      const ok = await login();
      if (!ok) throw new Error('Prijava je otkazana.');
    }
    try {
      return await api(path, { method: 'POST', body });
    } catch (e) {
      if (e.status === 401) { setStoredAuth(null); continue; }
      throw e;
    }
  }
  throw new Error('Prijava nije uspela.');
}

function login() {
  return new Promise((resolve) => {
    const dlg = $('#login-dialog');
    const form = $('#login-form');
    const pass = $('#login-pass');
    const errEl = $('#login-error');
    errEl.textContent = '';
    pass.value = '';

    const finish = (result) => {
      form.onsubmit = null;
      $('#login-cancel').onclick = null;
      dlg.close();
      resolve(result);
    };
    $('#login-cancel').onclick = () => finish(false);
    form.onsubmit = async (ev) => {
      ev.preventDefault();
      const token = btoa(unescape(encodeURIComponent('admin:' + pass.value)));
      try {
        const res = await fetch('/api/v1/auth', { method: 'POST', headers: { Authorization: 'Basic ' + token } });
        const data = await res.json().catch(() => ({}));
        if (!res.ok) { errEl.textContent = data.error || 'Pogrešna lozinka.'; return; }
        setStoredAuth(token);
        finish(true);
      } catch (e) {
        errEl.textContent = 'Uređaj nije dostupan.';
      }
    };
    dlg.showModal();
    pass.focus();
  });
}

function updateAuthButton() {
  const btn = $('#auth-btn');
  const logged = !!storedAuth();
  btn.textContent = logged ? '🔓' : '🔒';
  btn.title = logged ? 'Odjava' : 'Prijava';
}

// ---------------------------------------------------------------------------
// Zaglavlje: vreme uređaja (specifikacija §12.5)
// ---------------------------------------------------------------------------

function deviceParts(ms) {
  const parts = {};
  new Intl.DateTimeFormat('en-GB', {
    timeZone: state.timezone, weekday: 'short', year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
  }).formatToParts(new Date(ms)).forEach((p) => { parts[p.type] = p.value; });
  const wd = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'].indexOf(parts.weekday);
  return { ...parts, wd };
}

function renderClock() {
  const s = state.status;
  const text = $('#clock-text');
  const dot = $('#time-dot');
  const clock = $('#clock');
  if (!s) return;
  const t = s.time;
  const cls = { synced: 'ok', stale: 'warn', manual: 'warn', syncing: 'sync' }[t.status] || '';
  dot.className = 'dot ' + cls;
  const statusText = {
    not_set: 'vreme nije podešeno', syncing: 'sinhronizacija u toku', synced: 'sinhronizovano (NTP)',
    stale: 'poslednja sinhronizacija starija od 24 h', manual: 'ručno podešeno',
  }[t.status] || t.status;
  clock.title = 'Status: ' + statusText;

  if (!t.valid || state.clockOffsetMs == null) {
    text.textContent = 'Vreme nije sinhronizovano';
    return;
  }
  const p = deviceParts(Date.now() + state.clockOffsetMs);
  // Upozorenje ako se vreme uređaja i pregledača razlikuje više od 2 minuta (specifikacija §12.5)
  const drift = Math.abs(state.clockOffsetMs) > 120000;
  text.textContent = `${DAYS[p.wd]}, ${p.day}.${p.month}.${p.year}.  ${p.hour}:${p.minute}:${p.second} ${t.tz_abbrev}${drift ? ' ⚠' : ''}`;
  if (drift) clock.title += ' · vreme uređaja se razlikuje od vremena ovog računara više od 2 min (Podešavanja → Preuzmi vreme iz pregledača)';
}

async function refreshStatus() {
  try {
    const s = await api('/status');
    state.status = s;
    state.timezone = s.time.timezone || state.timezone;
    if (s.time.valid) state.clockOffsetMs = s.time.epoch_ms - Date.now();
    renderClock();
    const year = s.time.valid ? s.time.local.slice(0, 4) : new Date().getFullYear();
    $('#sidebar-foot').innerHTML = `Made by <b>Zoomtronic</b><br>verzija ${esc(s.firmware)} · ${esc(year)}`;
  } catch (e) {
    $('#clock-text').textContent = 'Uređaj nije dostupan';
    $('#time-dot').className = 'dot';
  }
}

// ---------------------------------------------------------------------------
// Stranice
// ---------------------------------------------------------------------------

function cardHtml(m) {
  const value = m.state === 'ok' ? esc(m.value) : (m.state === 'unknown' ? 'nepoznato' : 'nedostupno');
  const unit = m.state === 'ok' && m.unit ? `<span class="unit">${esc(m.unit)}</span>` : '';
  let foot = m.state === 'ok' ? `ažurirano ${ago(m.age_s)}` : esc(m.reason || 'čeka se prvo merenje');
  if (m.key === 'motion' && m.state === 'ok') {
    return `<div class="card"><div class="head"><span>${esc(m.name)}</span><span class="icon">${ICONS[m.key] || ''}</span></div>
      <div class="value">${m.value > 0.5 ? 'DA' : 'ne'}</div><div class="foot">${foot}</div></div>`;
  }
  return `<div class="card state-${m.state}"><div class="head"><span>${esc(m.name)}</span><span class="icon">${ICONS[m.key] || ''}</span></div>
    <div class="value">${value}${unit}</div><div class="foot">${foot}</div></div>`;
}

// ---------------------------------------------------------------------------
// Instrumenti (gauge) - specifikacija §6.2
// Zone boja samo prema zvaničnim skalama: PM2.5/PM10 i nivo prema EAQI, UV indeks prema WHO.
// Ostali instrumenti su neutralni (bez izmišljenih pragova).
// ---------------------------------------------------------------------------

const EAQI_PM25 = [[5, AQ_COLORS[1]], [15, AQ_COLORS[2]], [50, AQ_COLORS[3]], [90, AQ_COLORS[4]], [140, AQ_COLORS[5]], [Infinity, AQ_COLORS[6]]];
const EAQI_PM10 = [[15, AQ_COLORS[1]], [45, AQ_COLORS[2]], [120, AQ_COLORS[3]], [195, AQ_COLORS[4]], [270, AQ_COLORS[5]], [Infinity, AQ_COLORS[6]]];
const WHO_UV = [[2.5, '#3EA72D'], [5.5, '#FFF300'], [7.5, '#F18B00'], [10.5, '#E53210'], [Infinity, '#B567A4']];

const GAUGES = {
  temperature: { min: -20, max: 45 },
  humidity: { min: 0, max: 100 },
  pressure: { min: 950, max: 1050 },
  gas_resistance: { min: 1000, max: 500000, log: true, scale: 0.001, unit: 'kΩ', decimals: 1 },
  pm1_0: { min: 0, max: 150 },
  pm2_5: { min: 0, max: 150, zones: EAQI_PM25 },
  pm4: { min: 0, max: 150 },
  pm10: { min: 0, max: 300, zones: EAQI_PM10 },
  typical_particle_size: { min: 0, max: 3 },
  uv_index: { min: 0, max: 12, zones: WHO_UV },
  illuminance: { min: 0, max: 100000, log: true },
  pitch: { min: -90, max: 90 },
  roll: { min: -180, max: 180 },
};

const G_START = 150, G_SWEEP = 240, G_CX = 60, G_CY = 60;

function polar(r, deg) {
  const a = deg * Math.PI / 180;
  return [G_CX + r * Math.cos(a), G_CY + r * Math.sin(a)];
}

function arcPath(r, fromFrac, toFrac) {
  const a0 = G_START + G_SWEEP * fromFrac, a1 = G_START + G_SWEEP * toFrac;
  const [x0, y0] = polar(r, a0), [x1, y1] = polar(r, a1);
  return `M${x0.toFixed(2)} ${y0.toFixed(2)} A${r} ${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}

function gaugeFrac(v, g) {
  const f = g.log
    ? (Math.log10(Math.max(v, g.min) + 1) - Math.log10(g.min + 1)) / (Math.log10(g.max + 1) - Math.log10(g.min + 1))
    : (v - g.min) / (g.max - g.min);
  return Math.max(0, Math.min(1, f));
}

function zoneColor(v, zones) {
  for (const [to, color] of zones) if (v <= to) return color;
  return zones[zones.length - 1][1];
}

function getNeedles() {
  try { return localStorage.getItem('gaugeNeedles') === '1'; } catch (e) { return false; }
}

function setNeedles(on) {
  try { localStorage.setItem('gaugeNeedles', on ? '1' : '0'); } catch (e) { /* nije dostupno */ }
}

// Zajednički SVG instrumenta; sa kazaljkom vrednost se ispisuje ispod centra
function gaugeSvg(o) {
  const needles = getNeedles();
  let body = `${o.zones}
      <path d="${arcPath(44, 0, 1)}" stroke="var(--border)" stroke-width="10" fill="none" stroke-linecap="round"/>`;
  if (o.ok && o.frac > 0.002) {
    body += `<path d="${arcPath(44, 0, o.frac)}" stroke="${o.color}" stroke-width="${needles ? 6 : 10}" fill="none" stroke-linecap="round"/>`;
  }
  if (needles) {
    // Podeoci na svakih 10 % skale
    for (let i = 0; i <= 10; i++) {
      const a = G_START + G_SWEEP * i / 10;
      const [x0, y0] = polar(36, a), [x1, y1] = polar(i % 5 === 0 ? 30 : 33, a);
      body += `<line x1="${x0.toFixed(1)}" y1="${y0.toFixed(1)}" x2="${x1.toFixed(1)}" y2="${y1.toFixed(1)}" stroke="var(--muted)" stroke-width="${i % 5 === 0 ? 1.5 : 1}"/>`;
    }
    if (o.ok) {
      const a = G_START + G_SWEEP * o.frac;
      const [xt, yt] = polar(40, a), [xb, yb] = polar(-8, a);
      body += `<line x1="${xb.toFixed(1)}" y1="${yb.toFixed(1)}" x2="${xt.toFixed(1)}" y2="${yt.toFixed(1)}" stroke="var(--crit)" stroke-width="2.5" stroke-linecap="round" class="needle"/>`;
    }
    body += `<circle cx="60" cy="60" r="5" fill="var(--text)"/>
      <text x="60" y="90" text-anchor="middle" class="g-val g-val-small">${esc(o.shown)} <tspan class="g-unit">${esc(o.unit)}</tspan></text>`;
  } else {
    body += `<text x="60" y="62" text-anchor="middle" class="g-val">${esc(o.shown)}</text>
      <text x="60" y="78" text-anchor="middle" class="g-unit">${esc(o.unit)}</text>`;
  }
  body += `<text x="22" y="100" text-anchor="middle" class="g-lim">${esc(o.minLabel)}</text>
      <text x="98" y="100" text-anchor="middle" class="g-lim">${esc(o.maxLabel)}</text>`;
  return `<svg viewBox="0 0 120 104" role="img" aria-label="${esc(o.label)}: ${esc(o.shown)} ${esc(o.unit)}">${body}</svg>`;
}

function gaugeHtml(m) {
  const g = GAUGES[m.key];
  const ok = m.state === 'ok';
  const raw = ok ? Number(m.value) : null;
  const shown = ok ? (g.scale ? (raw * g.scale).toFixed(g.decimals) : m.value) : '-';
  let zones = '';
  if (g.zones) {
    let from = g.min;
    for (const [to, c] of g.zones) {
      const end = Math.min(to, g.max);
      if (end > from) zones += `<path d="${arcPath(54, gaugeFrac(from, g), gaugeFrac(end, g))}" stroke="${c}" stroke-width="4" fill="none"/>`;
      from = end;
      if (end >= g.max) break;
    }
  }
  const fmt = (n) => (Math.abs(n) >= 1000 ? (n / 1000) + 'k' : String(n));
  const sub = ok ? `ažurirano ${ago(m.age_s)}` : esc(m.reason || (m.state === 'unknown' ? 'čeka se prvo merenje' : 'nedostupno'));
  const svg = gaugeSvg({
    ok, shown, zones, label: m.name,
    unit: g.unit || m.unit,
    frac: ok ? gaugeFrac(raw, g) : 0,
    color: ok ? (g.zones ? zoneColor(raw, g.zones) : 'var(--accent)') : 'var(--na)',
    minLabel: fmt(g.scale ? g.min * g.scale : g.min),
    maxLabel: fmt(g.scale ? g.max * g.scale : g.max),
  });
  return `<div class="card gauge state-${m.state}">
    <div class="head"><span>${esc(m.name)}</span><span class="icon">${ICONS[m.key] || ''}</span></div>
    ${svg}<div class="foot">${sub}</div></div>`;
}

function eaqiGaugeHtml(aq) {
  const lvl = aq.level || 0;
  let zones = '';
  for (let i = 1; i <= 6; i++) {
    zones += `<path d="${arcPath(54, (i - 1) / 6, i / 6)}" stroke="${AQ_COLORS[i]}" stroke-width="4" fill="none"/>`;
  }
  // Kazaljka pokazuje sredinu polja nivoa
  const svg = gaugeSvg({
    ok: lvl > 0, zones, label: 'Kvalitet vazduha',
    shown: lvl ? `${lvl}/6` : '-', unit: lvl ? aq.name : 'nepoznat',
    frac: lvl ? (getNeedles() ? (lvl - 0.5) / 6 : lvl / 6) : 0,
    color: AQ_COLORS[lvl] || 'var(--na)', minLabel: '1', maxLabel: '6',
  });
  return `<div class="card gauge">
    <div class="head"><span>Kvalitet vazduha (EAQI)</span><span class="icon">🍃</span></div>
    ${svg}<div class="foot">${lvl ? 'lokalna procena, klizni prosek 10 min' : 'nema svežih PM merenja'}</div></div>`;
}

// Wi-Fi signal: orijentacione granice kvaliteta (korisnički pragovi, ne standard)
const WIFI_ZONES = [[-80, '#e5534b', 'vrlo slab'], [-70, '#F18B00', 'slab'], [-60, '#d4c72c', 'dobar'], [-50, '#2da44e', 'vrlo dobar'], [Infinity, '#1b9aaa', 'odličan']];

function wifiGaugeHtml(w) {
  const g = { min: -100, max: -30 };
  const ok = w && w.connected && w.rssi != null;
  const zone = ok ? WIFI_ZONES.find(([to]) => w.rssi <= to) : null;
  let zones = '', from = g.min;
  for (const [to, c] of WIFI_ZONES) {
    const end = Math.min(to, g.max);
    zones += `<path d="${arcPath(54, gaugeFrac(from, g), gaugeFrac(end, g))}" stroke="${c}" stroke-width="4" fill="none"/>`;
    from = end;
  }
  const svg = gaugeSvg({
    ok, zones, label: 'Wi-Fi signal',
    shown: ok ? String(w.rssi) : '-', unit: ok ? `dBm · ${zone[2]}` : 'dBm',
    frac: ok ? gaugeFrac(w.rssi, g) : 0, color: ok ? zone[1] : 'var(--na)', minLabel: '-100', maxLabel: '-30',
  });
  return `<div class="card gauge state-${ok ? 'ok' : 'unavailable'}">
    <div class="head"><span>Wi-Fi signal</span><span class="icon">📶</span></div>
    ${svg}<div class="foot">${ok ? esc(w.ssid) : (w && w.portal ? 'konfiguracioni portal' : 'nije povezan')}</div></div>`;
}

// Grupe instrumenata na kontrolnoj tabli
const GAUGE_GROUPS = [
  { title: 'Klima', keys: ['temperature', 'humidity', 'pressure'] },
  { title: 'Čestice (PM)', keys: ['pm1_0', 'pm2_5', 'pm4', 'pm10', 'typical_particle_size'] },
  { title: 'Kvalitet vazduha', keys: ['air_quality_level', 'gas_resistance'] },
  { title: 'Svetlo i UV', keys: ['uv_index', 'illuminance'] },
  { title: 'Položaj', keys: ['pitch', 'roll', 'motion'] },
  { title: 'Uređaj', keys: ['wifi_rssi'] },
];

function getDashView() {
  try { return localStorage.getItem('dashView') || 'cards'; } catch (e) { return 'cards'; }
}

function setDashView(v) {
  try { localStorage.setItem('dashView', v); } catch (e) { /* nije dostupno */ }
}

async function pageDashboard(main) {
  const [r, s] = await Promise.all([api('/readings'), api('/status')]);
  const view = getDashView();
  const toggle = `<div class="seg" role="group" aria-label="Prikaz">
      <button class="${view === 'cards' ? 'active' : ''}" data-view="cards">Kartice</button>
      <button class="${view === 'gauges' ? 'active' : ''}" data-view="gauges">Instrumenti</button></div>`;
  const bindToggle = () => main.querySelectorAll('.seg button').forEach((b) => {
    b.onclick = () => { setDashView(b.dataset.view); pageDashboard(main); };
  });

  if (view === 'gauges') {
    const byKey = Object.fromEntries(r.metrics.map((m) => [m.key, m]));
    const sections = GAUGE_GROUPS.map((grp) => {
      const items = grp.keys.map((k) => {
        if (k === 'air_quality_level') return eaqiGaugeHtml(r.air_quality);
        if (k === 'wifi_rssi') return wifiGaugeHtml(s.wifi);
        const m = byKey[k];
        if (!m) return '';
        return GAUGES[k] ? gaugeHtml(m) : cardHtml(m);
      }).join('');
      return `<section class="g-section"><h3>${esc(grp.title)}</h3><div class="grid gauges">${items}</div></section>`;
    }).join('');
    main.innerHTML = `<div class="page-head"><h2>Kontrolna tabla</h2>
        <div class="actions"><label class="check small"><input type="checkbox" id="needles" ${getNeedles() ? 'checked' : ''}> Kazaljke</label>${toggle}</div></div>
      ${sections}
      <p class="muted small">Boje zona: PM2.5, PM10 i nivo kvaliteta vazduha prema EAQI (lokalna procena), UV indeks prema skali WHO;
        Wi-Fi signal prema orijentacionim (korisničkim) granicama. Ostali instrumenti nemaju pragove. Gasni otpor i osvetljenost su na logaritamskoj skali.</p>`;
    bindToggle();
    $('#needles').onchange = (ev) => { setNeedles(ev.target.checked); pageDashboard(main); };
    return;
  }
  const aq = r.air_quality;
  const aqCard = `<div class="card aq" style="--aq:${AQ_COLORS[aq.level] || 'var(--na)'}">
      <div class="head"><span>Kvalitet vazduha (EAQI)</span><span class="icon">🍃</span></div>
      <div class="value">${aq.level ? esc(aq.name) : 'nepoznat'}${aq.level ? `<span class="unit">${aq.level}/6</span>` : ''}</div>
      <div class="foot">${aq.level ? `lokalna procena, ${esc(aq.dominant)} 10 min: ${esc(aq.dominant_avg_10min)} µg/m³` : 'nema svežih PM merenja'}</div></div>`;
  const cards = r.metrics.filter((m) => m.key !== 'air_quality_level' && m.key !== 'roll').map((m) => {
    if (m.key === 'pitch') {
      const roll = r.metrics.find((x) => x.key === 'roll');
      const ok = m.state === 'ok' && roll && roll.state === 'ok';
      return `<div class="card state-${ok ? 'ok' : 'unavailable'}"><div class="head"><span>Nagib (pitch / roll)</span><span class="icon">📐</span></div>
        <div class="value">${ok ? `${esc(m.value)}° / ${esc(roll.value)}°` : 'nedostupno'}</div><div class="foot">${ok ? 'ažurirano ' + ago(m.age_s) : ''}</div></div>`;
    }
    return cardHtml(m);
  }).join('');
  const sensorsOk = s.sensors.ok === s.sensors.total;
  const statusCard = `<div class="card"><div class="head"><span>Status uređaja</span><span class="icon">⚙️</span></div>
      <div class="value" style="font-size:20px">${s.sensors.ok}/${s.sensors.total} <span class="unit">senzora</span></div>
      <div class="foot">${sensorsOk ? 'svi senzori ispravni' : 'proveriti stranicu Senzori'} · Wi-Fi ${s.wifi.connected ? esc(s.wifi.ip) : (s.wifi.portal ? 'portal' : 'nije povezan')}</div></div>`;
  main.innerHTML = `<div class="page-head"><h2>Kontrolna tabla</h2>${toggle}</div><div class="grid">${aqCard}${cards}${statusCard}</div>
    <p class="muted small">Vrednosti su lokalno izmerene; nivo kvaliteta vazduha je lokalna procena po skali EAQI, a ne zvanični indeks.</p>`;
  bindToggle();
}

function pinsText(pins) {
  return (pins || []).map((p) => `${esc(p.role)} → GPIO${p.gpio} (${esc(p.label)})`).join('<br>');
}

function statusBadge(st, text) {
  const cls = { ok: 'ok', initializing: 'warn', not_found: 'crit', error: 'crit', disabled: '' }[st] || '';
  return `<span class="badge ${cls}">${esc(text)}</span>`;
}

async function pageSensors(main) {
  const d = await api('/sensors');
  const rows = d.sensors.map((s) => `<tr><td><b>${esc(s.name)}</b><br><span class="muted small">${esc(s.id)}</span></td>
      <td>${esc(s.interface)}</td><td>${pinsText(s.pins)}</td><td>${esc(s.i2c_address)}</td><td>${s.supply_v} V</td>
      <td>${statusBadge(s.status, s.status_text)}</td><td>${s.error_count}</td>
      <td>${s.last_reading_age_s != null ? ago(s.last_reading_age_s) : '-'}</td></tr>`).join('');
  const per = d.peripherals.map((p) => `<tr><td><b>${esc(p.name)}</b></td><td>${esc(p.interface)}</td><td>${pinsText(p.pins)}</td>
      <td>${esc(p.i2c_address || '-')}</td><td>${p.supply_v ? p.supply_v + ' V' : '-'}</td><td>${statusBadge(p.status, p.status_text)}</td></tr>`).join('');
  const warn = [];
  if (!d.buses.i2c0_pullups_ok) warn.push('Na glavnoj I2C magistrali (GPIO21/22) nisu pronađeni pull-up otpornici.');
  if (!d.buses.i2c1_pullups_ok) warn.push('Na I2C1 magistrali za SPS30 (GPIO16/17) nisu pronađeni pull-up otpornici - dodati 4,7-10 kΩ prema 3,3 V.');
  main.innerHTML = `<h2>Senzori</h2>
    ${warn.map((w) => `<div class="notice warn">⚠️ ${esc(w)}</div>`).join('')}
    <div class="panel table-wrap"><h3>Senzori</h3><table>
      <tr><th>Senzor</th><th>Interfejs</th><th>Pinovi kontrolera</th><th>Adresa</th><th>Napajanje</th><th>Status</th><th>Greške</th><th>Poslednje očitavanje</th></tr>
      ${rows}</table></div>
    <div class="panel table-wrap"><h3>Periferije</h3><table>
      <tr><th>Komponenta</th><th>Interfejs</th><th>Pinovi kontrolera</th><th>Adresa</th><th>Napajanje</th><th>Status</th></tr>
      ${per}</table></div>`;
}

async function pageWifi(main) {
  const w = await api('/wifi');
  const s = state.status || (await api('/status'));
  const modeText = { connected: 'povezan', connecting: 'povezivanje…', portal: 'konfiguracioni portal', off: 'isključen' }[w.mode];
  const firstSetup = w.portal_client && !s.admin_password_set;
  main.innerHTML = `<h2>Wi-Fi</h2>
    ${w.portal_active ? `<div class="notice">Aktivan je konfiguracioni portal (mreža <b>${esc(w.ap_ssid)}</b>). Izaberite mrežu i unesite lozinku.</div>` : ''}
    <div class="panel"><h3>Status</h3><table>
      <tr><td class="k">Stanje</td><td>${esc(modeText)}</td></tr>
      <tr><td class="k">Mreža (SSID)</td><td>${esc(w.ssid || '-')}</td></tr>
      <tr><td class="k">IP adresa</td><td>${esc(w.ip || '-')}</td></tr>
      <tr><td class="k">Gateway / maska</td><td>${esc(w.gateway || '-')} / ${esc(w.subnet || '-')}</td></tr>
      <tr><td class="k">DNS</td><td>${esc(w.dns || '-')}</td></tr>
      <tr><td class="k">Signal (RSSI)</td><td>${w.rssi != null ? w.rssi + ' dBm' : '-'}</td></tr>
      <tr><td class="k">Povezan</td><td>${w.connected_for_s != null ? duration(w.connected_for_s) : '-'}</td></tr>
      <tr><td class="k">Naziv uređaja</td><td>${esc(w.hostname)} (<a href="http://${esc(w.hostname)}.local/">${esc(w.hostname)}.local</a>)</td></tr>
      <tr><td class="k">MAC adresa</td><td>${esc(w.mac)}</td></tr>
      <tr><td class="k">Ponovnih povezivanja</td><td>${w.reconnects}</td></tr>
    </table></div>
    <div class="panel"><h3>Podešavanje mreže</h3>
      <div class="actions"><button class="btn ghost" id="scan-btn">Pretraži mreže</button></div>
      <ul class="net-list" id="net-list"></ul>
      <form class="settings" id="wifi-form">
        <label>Mreža (SSID)<input name="ssid" maxlength="32" required value="${esc(w.ssid)}"></label>
        <label>Lozinka mreže<input name="password" type="password" maxlength="64" autocomplete="new-password"
          placeholder="${w.credentials_set ? 'ostaviti prazno za postojeću lozinku' : ''}"></label>
        <label class="check"><input type="checkbox" name="static_ip" ${w.static_ip ? 'checked' : ''}> Statička IP adresa</label>
        <div id="static-fields" ${w.static_ip ? '' : 'hidden'} style="display:grid;gap:12px">
          <label>IP adresa<input name="ip" value="${esc(w.static.ip)}"></label>
          <label>Gateway<input name="gateway" value="${esc(w.static.gateway)}"></label>
          <label>Maska podmreže<input name="subnet" value="${esc(w.static.subnet)}"></label>
          <label>DNS<input name="dns" value="${esc(w.static.dns)}"></label>
        </div>
        <label class="check"><input type="checkbox" name="clear_mqtt"> Obriši MQTT podešavanja (nova lokacija / drugi Home Assistant)</label>
        ${firstSetup ? `<label>Nova administratorska lozinka (najmanje 8 znakova)
          <input name="admin_password" type="password" minlength="8" maxlength="64" autocomplete="new-password"></label>` : ''}
        <div class="row-end"><button class="btn" type="submit">Sačuvaj i poveži</button></div>
      </form>
    </div>`;

  const form = $('#wifi-form');
  form.static_ip.onchange = () => { $('#static-fields').hidden = !form.static_ip.checked; };
  $('#scan-btn').onclick = () => scanNetworks(form);
  form.onsubmit = async (ev) => {
    ev.preventDefault();
    const body = { ssid: form.ssid.value.trim(), static_ip: form.static_ip.checked };
    if (form.password.value) body.password = form.password.value;
    if (body.static_ip) ['ip', 'gateway', 'subnet', 'dns'].forEach((k) => { body[k] = form[k].value.trim(); });
    if (form.admin_password && form.admin_password.value) body.admin_password = form.admin_password.value;
    if (form.clear_mqtt.checked) body.clear_mqtt = true;
    try {
      const res = w.portal_client
        ? await api('/wifi', { method: 'POST', body })
        : await adminApi('/wifi', body);
      banner(res.message);
    } catch (e) { banner(e.message, true); }
  };
}

async function scanNetworks(form) {
  const list = $('#net-list');
  list.innerHTML = '<li class="muted">Pretraga…</li>';
  try {
    await api('/wifi/scan?refresh=1');
    for (let i = 0; i < 15; i++) {
      await new Promise((r) => setTimeout(r, 1000));
      const d = await api('/wifi/scan');
      if (!d.scanning) {
        const nets = (d.networks || []).sort((a, b) => b.rssi - a.rssi);
        list.innerHTML = nets.length ? nets.map((n) =>
          `<li data-ssid="${esc(n.ssid)}"><span>${n.secure ? '🔒' : '🔓'} ${esc(n.ssid)}</span><span class="muted">${n.rssi} dBm</span></li>`).join('')
          : '<li class="muted">Nije pronađena nijedna mreža.</li>';
        list.querySelectorAll('li[data-ssid]').forEach((li) => {
          li.onclick = () => { form.ssid.value = li.dataset.ssid; form.password.focus(); };
        });
        return;
      }
    }
    list.innerHTML = '<li class="muted">Pretraga nije završena.</li>';
  } catch (e) {
    list.innerHTML = `<li class="error">${esc(e.message)}</li>`;
  }
}

async function pageSettings(main) {
  const [c, t] = await Promise.all([api('/config'), api('/time')]);
  main.innerHTML = `<h2>Podešavanja</h2>
    <div class="panel"><h3>Prikaz</h3>
      <form class="settings" id="display-form">
        <label>Osvetljenje OLED ekrana: <b id="oled-val">${c.oled_brightness} %</b>
          <input type="range" name="oled_brightness" min="0" max="100" step="5" value="${c.oled_brightness}"></label>
        <p class="muted small">0 % isključuje ekran. Promena se primenjuje odmah.</p>
        <label class="check"><input type="checkbox" name="status_led_enabled" ${c.status_led_enabled ? 'checked' : ''}>
          Statusna LED dioda na ploči uključena</label>
        <p class="muted small">Plava LED kodom treptanja pokazuje stanje uređaja (1 bljesak / 2 s = sve ispravno).
          Kada je isključena, stanje se i dalje vidi na stranici Sistem.</p>
      </form>
    </div>
    <div class="panel"><h3>Vreme i NTP</h3>
      <p class="small">Status: <b>${esc({ not_set: 'nije podešeno', syncing: 'sinhronizacija u toku', synced: 'sinhronizovano', stale: 'zastarelo', manual: 'ručno podešeno' }[t.status])}</b>
        ${t.last_sync_utc ? ` · poslednja sinhronizacija ${esc(new Date(t.last_sync_utc).toLocaleString('sr-Latn-RS', { timeZone: c.timezone }))}` : ''}</p>
      <form class="settings" id="time-form">
        <label class="check"><input type="checkbox" name="ntp_enabled" ${c.ntp_enabled ? 'checked' : ''}> NTP sinhronizacija uključena</label>
        <label>Primarni NTP server<input name="ntp_server1" maxlength="63" value="${esc(c.ntp_server1)}"></label>
        <label>Sekundarni NTP server<input name="ntp_server2" maxlength="63" value="${esc(c.ntp_server2)}"></label>
        <label>Rezervni NTP server<input name="ntp_server3" maxlength="63" value="${esc(c.ntp_server3)}"></label>
        <label>Interval sinhronizacije (min)<input name="ntp_interval_min" type="number" min="15" max="1440" value="${c.ntp_interval_min}"></label>
        <label>Vremenska zona<select name="timezone">${c.timezones.map((z) => `<option ${z === c.timezone ? 'selected' : ''}>${esc(z)}</option>`).join('')}</select></label>
        <div class="row-end">
          <button type="button" class="btn ghost" id="browser-time">Preuzmi vreme iz pregledača</button>
          <button class="btn" type="submit">Sačuvaj</button>
        </div>
      </form>
    </div>
    <div class="panel"><h3>Pristup</h3>
      <form class="settings" id="access-form">
        <label>Naziv uređaja u mreži<input name="hostname" maxlength="31" value="${esc(c.hostname)}"></label>
        <label>Nova administratorska lozinka<input name="admin_password" type="password" minlength="8" maxlength="64" autocomplete="new-password"
          placeholder="${c.admin_password_set ? 'lozinka je podešena' : 'još se koristi lozinka pristupne tačke'}"></label>
        <label>Nova lozinka pristupne tačke (portal)<input name="ap_password" type="password" minlength="8" maxlength="63" autocomplete="new-password"
          placeholder="${c.ap_password_custom ? 'promenjena' : 'podrazumevana, prikazuje se na OLED-u'}"></label>
        <div class="row-end"><button class="btn" type="submit">Sačuvaj</button></div>
      </form>
    </div>`;

  const df = $('#display-form');
  let brightnessTimer = null;
  df.oled_brightness.oninput = () => {
    $('#oled-val').textContent = df.oled_brightness.value + ' %';
    clearTimeout(brightnessTimer);
    brightnessTimer = setTimeout(async () => {
      try { await adminApi('/config', { oled_brightness: Number(df.oled_brightness.value) }); }
      catch (e) { banner(e.message, true); }
    }, 300);
  };

  df.status_led_enabled.onchange = async () => {
    try {
      const res = await adminApi('/config', { status_led_enabled: df.status_led_enabled.checked });
      banner(res.message);
    } catch (e) {
      df.status_led_enabled.checked = !df.status_led_enabled.checked;
      banner(e.message, true);
    }
  };

  const tf = $('#time-form');
  tf.onsubmit = async (ev) => {
    ev.preventDefault();
    try {
      const res = await adminApi('/config', {
        ntp_enabled: tf.ntp_enabled.checked, ntp_server1: tf.ntp_server1.value.trim(), ntp_server2: tf.ntp_server2.value.trim(),
        ntp_server3: tf.ntp_server3.value.trim(), ntp_interval_min: Number(tf.ntp_interval_min.value), timezone: tf.timezone.value,
      });
      banner(res.message);
    } catch (e) { banner(e.message, true); }
  };
  $('#browser-time').onclick = async () => {
    try {
      const res = await adminApi('/time', { epoch: Math.floor(Date.now() / 1000) });
      banner(res.message);
      setTimeout(refreshStatus, 500);
    } catch (e) { banner(e.message, true); }
  };

  const af = $('#access-form');
  af.onsubmit = async (ev) => {
    ev.preventDefault();
    const body = { hostname: af.hostname.value.trim() };
    if (af.admin_password.value) body.admin_password = af.admin_password.value;
    if (af.ap_password.value) body.ap_password = af.ap_password.value;
    try {
      const res = await adminApi('/config', body);
      if (body.admin_password) setStoredAuth(btoa(unescape(encodeURIComponent('admin:' + body.admin_password))));
      banner(res.message);
      af.admin_password.value = '';
      af.ap_password.value = '';
    } catch (e) { banner(e.message, true); }
  };
}

// Traka zauzeća; pragovi boja (70 % / 90 %) su korisnički, ne zvanični
function resourceBar(title, pct, detail, invert = false) {
  const p = Math.max(0, Math.min(100, pct));
  const level = invert ? 100 - p : p;
  const cls = level >= 90 ? 'crit' : level >= 70 ? 'warn' : '';
  return `<div><div class="res-head"><span>${esc(title)}</span><span>${esc(detail)}</span></div>
    <div class="bar ${cls}" role="progressbar" aria-valuenow="${Math.round(p)}" aria-valuemin="0" aria-valuemax="100">
      <span style="width:${p.toFixed(1)}%"></span></div></div>`;
}

async function pageMqtt(main) {
  const q = await api('/mqtt');
  const conn = !q.enabled ? '<span class="badge">isključen</span>'
    : q.connected ? '<span class="badge ok">povezan</span>'
      : `<span class="badge crit">nije povezan</span>${q.error ? ` <span class="small muted">(${esc(q.error)})</span>` : ''}`;
  main.innerHTML = `<h2>MQTT</h2>
    <div class="panel"><h3>Status</h3><table>
      <tr><td class="k">Veza</td><td>${conn}</td></tr>
      <tr><td class="k">Broker</td><td>${q.host ? esc(q.host) + ':' + q.port : '-'}</td></tr>
      <tr><td class="k">Topic stanja (JSON)</td><td><code>${esc(q.state_topic)}</code></td></tr>
      <tr><td class="k">Availability (LWT)</td><td><code>${esc(q.availability_topic)}</code> - online / offline</td></tr>
      <tr><td class="k">Pojedinačni topic-i</td><td><code>${esc(q.base_topic)}/temperature</code>, <code>…/pm2_5</code>, …</td></tr>
      <tr><td class="k">Objava</td><td>${q.publish_count} objava${q.publish_count ? ', poslednja ' + ago(q.last_publish_age_s) : ''} · povezivanja: ${q.connect_count}</td></tr>
      <tr><td class="k">Home Assistant Discovery</td><td>${q.discovery ? `uključen (prefiks <code>${esc(q.discovery_prefix)}</code>)` : 'isključen'}</td></tr>
    </table></div>
    <div class="panel"><h3>Podešavanja</h3>
      <form class="settings" id="mqtt-form">
        <label class="check"><input type="checkbox" name="enabled" ${q.enabled ? 'checked' : ''}> MQTT uključen</label>
        <label>Adresa brokera (IP ili naziv)<input name="host" maxlength="63" value="${esc(q.host)}" placeholder="npr. 192.168.88.10 ili homeassistant.local"></label>
        <label>Port<input name="port" type="number" min="1" max="65535" value="${q.port}"></label>
        <label>Korisničko ime<input name="username" maxlength="32" value="${esc(q.username)}" autocomplete="off"></label>
        <label>Lozinka<input name="password" type="password" maxlength="64" autocomplete="new-password"
          placeholder="${q.password_set ? 'ostaviti prazno za postojeću lozinku' : ''}"></label>
        <label>Client ID<input name="client_id" maxlength="32" value="${esc(q.client_id)}"></label>
        <label>Osnovni topic<input name="base_topic" maxlength="63" value="${esc(q.base_topic)}"></label>
        <label>Interval objavljivanja (s)<input name="interval_s" type="number" min="5" max="300" value="${q.interval_s}"></label>
        <label>QoS<select name="qos"><option value="0" ${q.qos === 0 ? 'selected' : ''}>0 - najviše jednom</option>
          <option value="1" ${q.qos === 1 ? 'selected' : ''}>1 - najmanje jednom</option></select></label>
        <label class="check"><input type="checkbox" name="discovery" ${q.discovery ? 'checked' : ''}> Home Assistant MQTT Discovery</label>
        <label>Discovery prefiks<input name="discovery_prefix" maxlength="31" value="${esc(q.discovery_prefix)}"></label>
        <p class="muted small">TLS veza još nije podržana; koristiti broker u lokalnoj mreži (port 1883).</p>
        <div class="row-end"><button class="btn" type="submit">Sačuvaj</button></div>
      </form>
    </div>
    <div class="panel"><h3>Brisanje MQTT podešavanja</h3>
      <p class="small">Za prelazak na drugi broker ili drugi Home Assistant: briše adresu brokera, korisnika i lozinku i isključuje MQTT.
        Wi-Fi i ostala podešavanja ostaju.</p>
      <label class="check"><input type="checkbox" id="mqtt-remove-ha" checked> Ukloni uređaj iz trenutnog Home Assistant-a</label>
      <p class="muted small">Uklanjanje radi samo dok je MQTT povezan: uređaj šalje prazne Discovery poruke i Home Assistant briše entitete.</p>
      <div class="actions"><button class="btn danger" id="mqtt-clear">Obriši MQTT podešavanja</button></div>
    </div>`;
  $('#mqtt-clear').onclick = async () => {
    if (!confirm('Obrisati MQTT podešavanja?')) return;
    try {
      banner((await adminApi('/mqtt/clear', { remove_from_ha: $('#mqtt-remove-ha').checked })).message);
      setTimeout(() => { if (state.page === 'mqtt') pageMqtt(main); }, 2500);
    } catch (e) { banner(e.message, true); }
  };
  const f = $('#mqtt-form');
  f.onsubmit = async (ev) => {
    ev.preventDefault();
    const body = {
      enabled: f.enabled.checked, host: f.host.value.trim(), port: Number(f.port.value), username: f.username.value.trim(),
      client_id: f.client_id.value.trim(), base_topic: f.base_topic.value.trim(), interval_s: Number(f.interval_s.value),
      qos: Number(f.qos.value), discovery: f.discovery.checked, discovery_prefix: f.discovery_prefix.value.trim(),
    };
    if (f.password.value) body.password = f.password.value;
    try {
      banner((await adminApi('/mqtt', body)).message);
      setTimeout(() => { if (state.page === 'mqtt') pageMqtt(main); }, 4000);
    } catch (e) { banner(e.message, true); }
  };
}

async function pageSystem(main) {
  const s = await api('/system');
  const heapUsed = s.heap_total - s.heap_free;
  const bars = [
    resourceBar('RAM (heap)', heapUsed / s.heap_total * 100,
      `${bytes(heapUsed)} / ${bytes(s.heap_total)} · ${Math.round(heapUsed / s.heap_total * 100)} %`),
    resourceBar('Najmanje slobodne RAM memorije od pokretanja', (s.heap_total - s.heap_min_free) / s.heap_total * 100,
      `slobodno najmanje ${bytes(s.heap_min_free)}`),
    resourceBar('Firmware (OTA particija)', s.sketch_bytes / s.ota_partition_bytes * 100,
      `${bytes(s.sketch_bytes)} / ${bytes(s.ota_partition_bytes)} · ${Math.round(s.sketch_bytes / s.ota_partition_bytes * 100)} %`),
    resourceBar('LittleFS (web i istorija)', s.fs_used / s.fs_total * 100,
      `${bytes(s.fs_used)} / ${bytes(s.fs_total)} · ${Math.round(s.fs_used / s.fs_total * 100)} %`),
  ];
  if (s.rssi != null) {
    // RSSI -90 dBm (slabo) .. -30 dBm (odlično) preslikano na 0-100 %
    const q = Math.max(0, Math.min(100, (s.rssi + 90) / 60 * 100));
    bars.push(resourceBar('Jačina Wi-Fi signala', q, `${s.rssi} dBm · ${Math.round(q)} %`, true));
  }
  main.innerHTML = `<h2>Sistem</h2>
    <div class="panel"><h3>Resursi</h3><div class="resources">${bars.join('')}</div></div>
    <div class="panel"><table>
      <tr><td class="k">Uređaj</td><td>${esc(s.device)}</td></tr>
      <tr><td class="k">Verzija firmware-a</td><td>${esc(s.firmware)}</td></tr>
      <tr><td class="k">Čip</td><td>${esc(s.chip)} ${esc(s.chip_revision)}, ${s.cores} jezgra, ${s.cpu_mhz} MHz
        <span class="muted"> - ${s.cpu_mhz > 200 ? 'opa, kao Pentium kec 😊' : 'još samo malo overclocka i možda bude Pentium kec 😊'}</span></td></tr>
      <tr><td class="k">Flash</td><td>${bytes(s.flash_bytes)} · firmware ${bytes(s.sketch_bytes)} od ${bytes(s.ota_partition_bytes)}</td></tr>
      <tr><td class="k">LittleFS</td><td>${bytes(s.fs_used)} / ${bytes(s.fs_total)}</td></tr>
      <tr><td class="k">Istorija</td><td>${s.history_5min} / ${s.history_5min_capacity} zapisa po 5 min ·
        ${s.history_hour} / ${s.history_hour_capacity} satnih zapisa</td></tr>
      <tr><td class="k">Slobodna memorija</td><td>${bytes(s.heap_free)} (najmanje ${bytes(s.heap_min_free)}, najveći blok ${bytes(s.heap_max_block)})</td></tr>
      <tr><td class="k">Vreme rada</td><td>${duration(s.uptime_s)}</td></tr>
      <tr><td class="k">Razlog poslednjeg restarta</td><td>${esc(s.reset_reason)}</td></tr>
      <tr><td class="k">Broj pokretanja</td><td>${s.boot_count}</td></tr>
      <tr><td class="k">Statusna LED</td><td>${esc(s.status_led)}${s.status_led_enabled ? '' : ' (dioda isključena)'}</td></tr>
      <tr><td class="k">MAC adresa</td><td>${esc(s.mac)}</td></tr>
      <tr><td class="k">ESP-IDF / Arduino</td><td>${esc(s.sdk)} / ${esc(s.arduino)}</td></tr>
    </table></div>
    <div class="panel"><h3>Akcije</h3><div class="actions">
      <button class="btn ghost" id="portal-btn">Pokreni konfiguracioni portal</button>
      <button class="btn danger" id="restart-btn">Restartuj uređaj</button>
    </div></div>
    <div class="panel"><h3>Fabrička podešavanja</h3>
      <p class="small">Briše sačuvanu Wi-Fi mrežu, administratorsku lozinku i lozinku pristupne tačke, MQTT, NTP i podešavanja prikaza.
        Posle restarta uređaj pokreće konfiguracioni portal <b>METEO-ESP32-SETUP</b>; lozinka pristupne tačke je prikazana na OLED-u.
        Koristiti npr. kada se stanica prenosi u drugu Wi-Fi mrežu.</p>
      <label class="check"><input type="checkbox" id="factory-remove-ha" checked> Ukloni uređaj iz trenutnog Home Assistant-a (ako je MQTT povezan)</label>
      <div class="actions"><button class="btn danger" id="factory-btn">Vrati fabrička podešavanja</button></div>
    </div>`;
  $('#factory-btn').onclick = async () => {
    if (!confirm('Obrisati SVA podešavanja (Wi-Fi, lozinke, MQTT, NTP, prikaz) i restartovati uređaj?')) return;
    if (!confirm('Da li ste sigurni? Uređaj će izaći iz ove Wi-Fi mreže i pokrenuti portal za podešavanje.')) return;
    try {
      banner((await adminApi('/system/factory-reset', { remove_from_ha: $('#factory-remove-ha').checked })).message);
      setStoredAuth(null);
    } catch (e) { banner(e.message, true); }
  };
  $('#restart-btn').onclick = async () => {
    if (!confirm('Restartovati uređaj?')) return;
    try { banner((await adminApi('/system/restart')).message); } catch (e) { banner(e.message, true); }
  };
  $('#portal-btn').onclick = async () => {
    try { banner((await adminApi('/wifi/portal')).message); } catch (e) { banner(e.message, true); }
  };
}

async function pageAbout(main) {
  const s = await api('/system');
  main.innerHTML = `<h2>O uređaju</h2><div class="panel"><table>
      <tr><td class="k">Projekat</td><td>ESP32 R201 - pametna meteo stanica</td></tr>
      <tr><td class="k">Ploča</td><td>NodeMCU ESP-32S V1.1 (${esc(s.chip)} ${esc(s.chip_revision)})</td></tr>
      <tr><td class="k">Verzija firmware-a</td><td>${esc(s.firmware)}</td></tr>
      <tr><td class="k">Senzori</td><td>BME680, MPU6500, LTR-390, SPS30</td></tr>
      <tr><td class="k">Prikaz</td><td>OLED 0,96" 128×64 SSD1306, statusna LED</td></tr>
    </table></div>`;
}

// ---------------------------------------------------------------------------
// Istorija i grafikoni (specifikacija §6.3) - SVG bez spoljnih biblioteka
// ---------------------------------------------------------------------------

const CHART_GROUPS = [
  { title: 'Temperatura i vlažnost', keys: ['temperature', 'humidity'] },
  { title: 'Atmosferski pritisak', keys: ['pressure'] },
  { title: 'Čestice PM', keys: ['pm1_0', 'pm2_5', 'pm4', 'pm10'], hidden: ['pm1_0', 'pm4'] },
  { title: 'UV indeks i osvetljenost', keys: ['uv_index', 'illuminance'] },
  { title: 'Gasni otpor', keys: ['gas_resistance'] },
  { title: 'Kvalitet vazduha (EAQI)', keys: ['air_quality_level'] },
  { title: 'Nagib', keys: ['pitch', 'roll'] },
];
const CHART_COLORS = ['#1f6feb', '#e5534b', '#2da44e', '#d4a72c', '#8250df', '#1b9aaa'];
const HIST_PERIODS = [['1h', '1 sat'], ['6h', '6 sati'], ['24h', '24 sata'], ['7d', '7 dana'], ['30d', '30 dana']];

const hist = {
  period: (() => { try { return localStorage.getItem('histPeriod') || '24h'; } catch (e) { return '24h'; } })(),
  band: (() => { try { return localStorage.getItem('histBand') !== '0'; } catch (e) { return true; } })(),
  hidden: new Set(CHART_GROUPS.flatMap((g) => g.hidden || [])),
  data: null,
};

function fmtAxisTime(ts, period) {
  const p = deviceParts(ts * 1000);
  return (period === '7d' || period === '30d') ? `${p.day}.${p.month}.` : `${p.hour}:${p.minute}`;
}

function fmtFullTime(ts) {
  const p = deviceParts(ts * 1000);
  return `${DAYS[p.wd]}, ${p.day}.${p.month}.${p.year}. ${p.hour}:${p.minute}`;
}

function niceNum(v, dec) {
  return Number(v).toFixed(Math.min(dec, 2));
}

// Jedan grafikon: do dve ose (po jedinici), linija proseka, opcioni min/max opseg i tooltip
function drawChart(box, group, data) {
  const width = Math.max(300, box.clientWidth);
  const height = 230;
  const idx = Object.fromEntries(data.metrics.map((m, i) => [m.key, i]));
  const series = group.keys.filter((k) => idx[k] != null).map((k, si) => {
    const i = idx[k];
    const meta = data.metrics[i];
    return {
      key: k, name: meta.name, unit: meta.unit, decimals: meta.decimals, color: CHART_COLORS[si % CHART_COLORS.length],
      pts: data.points.map((p) => ({ t: p[0], avg: p[1 + i * 3], min: p[2 + i * 3], max: p[3 + i * 3] })),
    };
  });
  const visible = series.filter((s) => !hist.hidden.has(s.key));
  const units = [...new Set(series.map((s) => s.unit))].slice(0, 2);
  const m = { l: 50, r: units.length > 1 ? 50 : 14, t: 10, b: 26 };
  const w = width - m.l - m.r, h = height - m.t - m.b;
  const tMax = data.now, tMin = data.now - { '1h': 3600, '6h': 21600, '24h': 86400, '7d': 604800, '30d': 2592000 }[data.period];
  const x = (t) => m.l + (t - tMin) / (tMax - tMin) * w;

  const axes = units.map((u) => {
    let lo = Infinity, hi = -Infinity;
    visible.filter((s) => s.unit === u).forEach((s) => s.pts.forEach((p) => {
      if (p.avg == null) return;
      lo = Math.min(lo, hist.band && p.min != null ? p.min : p.avg);
      hi = Math.max(hi, hist.band && p.max != null ? p.max : p.avg);
    }));
    if (!isFinite(lo)) { lo = 0; hi = 1; }
    if (hi - lo < 1e-6) { lo -= 1; hi += 1; }
    const pad = (hi - lo) * 0.08;
    return { unit: u, lo: lo - pad, hi: hi + pad };
  });
  const yOf = (s) => {
    const a = axes.find((ax) => ax.unit === s.unit) || axes[0];
    return (v) => m.t + h - (v - a.lo) / (a.hi - a.lo) * h;
  };

  let svg = `<svg width="${width}" height="${height}" class="chart-svg">`;
  // Mreža i ose
  for (let i = 0; i <= 4; i++) {
    const yy = m.t + h * i / 4;
    svg += `<line x1="${m.l}" x2="${m.l + w}" y1="${yy}" y2="${yy}" class="grid"/>`;
    axes.forEach((a, ai) => {
      const v = a.hi - (a.hi - a.lo) * i / 4;
      const dec = Math.abs(a.hi - a.lo) < 10 ? 1 : 0;
      svg += `<text x="${ai === 0 ? m.l - 6 : m.l + w + 6}" y="${yy + 3}" text-anchor="${ai === 0 ? 'end' : 'start'}" class="tick">${v.toFixed(dec)}</text>`;
    });
  }
  axes.forEach((a, ai) => {
    svg += `<text x="${ai === 0 ? 4 : width - 4}" y="${m.t + 8}" text-anchor="${ai === 0 ? 'start' : 'end'}" class="tick unit">${esc(a.unit)}</text>`;
  });
  const ticks = width < 500 ? 4 : 6;
  for (let i = 0; i <= ticks; i++) {
    const t = tMin + (tMax - tMin) * i / ticks;
    svg += `<text x="${x(t)}" y="${height - 6}" text-anchor="${i === 0 ? 'start' : i === ticks ? 'end' : 'middle'}" class="tick">${fmtAxisTime(t, data.period)}</text>`;
  }

  // Serije: prekid linije gde nedostaju podaci (uređaj isključen ili senzor nedostupan)
  const gap = data.resolution_s * 2.5;
  visible.forEach((s) => {
    const y = yOf(s);
    let line = '', band = [], bands = [];
    let prevT = null;
    s.pts.forEach((p) => {
      if (p.avg == null || p.t < tMin) { prevT = null; return; }
      const cx = x(p.t + data.resolution_s / 2);
      const broken = prevT == null || p.t - prevT > gap;
      line += `${broken ? 'M' : 'L'}${cx.toFixed(1)} ${y(p.avg).toFixed(1)}`;
      if (broken && band.length) { bands.push(band); band = []; }
      if (p.min != null && p.max != null) band.push([cx, y(p.max), y(p.min)]);
      prevT = p.t;
    });
    if (band.length) bands.push(band);
    if (hist.band) {
      bands.forEach((b) => {
        const top = b.map((q) => `${q[0].toFixed(1)},${q[1].toFixed(1)}`).join(' ');
        const bot = b.slice().reverse().map((q) => `${q[0].toFixed(1)},${q[2].toFixed(1)}`).join(' ');
        svg += `<polygon points="${top} ${bot}" fill="${s.color}" opacity="0.15"/>`;
      });
    }
    svg += `<path d="${line}" fill="none" stroke="${s.color}" stroke-width="2" stroke-linejoin="round"/>`;
  });
  svg += `<line class="cursor" x1="0" x2="0" y1="${m.t}" y2="${m.t + h}" visibility="hidden"/>`;
  svg += `<rect class="hover" x="${m.l}" y="${m.t}" width="${w}" height="${h}" fill="transparent"/></svg>`;

  // Legenda (uključivanje/isključivanje serija) i pregled min/max/prosek
  const legend = series.map((s) => {
    const vals = s.pts.filter((p) => p.avg != null && p.t >= tMin);
    let summary = 'nema podataka';
    if (vals.length) {
      const lo = Math.min(...vals.map((p) => p.min ?? p.avg)), hi = Math.max(...vals.map((p) => p.max ?? p.avg));
      const avg = vals.reduce((a, p) => a + p.avg, 0) / vals.length;
      summary = `min ${niceNum(lo, s.decimals)} · max ${niceNum(hi, s.decimals)} · prosek ${niceNum(avg, s.decimals + 1)} ${esc(s.unit)}`;
    }
    return `<button class="legend-item ${hist.hidden.has(s.key) ? 'off' : ''}" data-key="${s.key}">
      <span class="swatch" style="background:${s.color}"></span>${esc(s.name)}<span class="muted small"> · ${summary}</span></button>`;
  }).join('');

  box.innerHTML = `${svg}<div class="chart-tip" hidden></div><div class="legend">${legend}</div>`;
  box.querySelectorAll('.legend-item').forEach((b) => {
    b.onclick = () => {
      if (hist.hidden.has(b.dataset.key)) hist.hidden.delete(b.dataset.key); else hist.hidden.add(b.dataset.key);
      drawChart(box, group, data);
    };
  });

  // Tooltip: najbliža tačka po vremenu
  const hover = box.querySelector('.hover'), cursor = box.querySelector('.cursor'), tip = box.querySelector('.chart-tip');
  const move = (clientX) => {
    const rect = box.querySelector('svg').getBoundingClientRect();
    const px = clientX - rect.left;
    const t = tMin + (px - m.l) / w * (tMax - tMin);
    let best = null;
    data.points.forEach((p) => { if (p[0] >= tMin && (best == null || Math.abs(p[0] - t) < Math.abs(best[0] - t))) best = p; });
    if (!best) return;
    const cx = x(best[0] + data.resolution_s / 2);
    cursor.setAttribute('x1', cx); cursor.setAttribute('x2', cx); cursor.setAttribute('visibility', 'visible');
    const rows = visible.map((s) => {
      const i = idx[s.key];
      const v = best[1 + i * 3];
      return `<div><span class="swatch" style="background:${s.color}"></span>${esc(s.name)}: <b>${v == null ? '-' : niceNum(v, s.decimals)} ${esc(s.unit)}</b>${v != null && best[2 + i * 3] != null ? ` <span class="muted">(${niceNum(best[2 + i * 3], s.decimals)}-${niceNum(best[3 + i * 3], s.decimals)})</span>` : ''}</div>`;
    }).join('');
    tip.innerHTML = `<div class="muted">${fmtFullTime(best[0])}</div>${rows}`;
    tip.hidden = false;
    const tipW = tip.offsetWidth;
    tip.style.left = Math.min(Math.max(cx - tipW / 2, 0), width - tipW) + 'px';
  };
  hover.addEventListener('mousemove', (e) => move(e.clientX));
  hover.addEventListener('touchmove', (e) => { move(e.touches[0].clientX); e.preventDefault(); }, { passive: false });
  hover.addEventListener('touchstart', (e) => move(e.touches[0].clientX), { passive: true });
  hover.addEventListener('mouseleave', () => { tip.hidden = true; cursor.setAttribute('visibility', 'hidden'); });
}

async function loadHistory() {
  const keys = [...new Set(CHART_GROUPS.flatMap((g) => g.keys))].join(',');
  hist.data = await api(`/history?period=${hist.period}&metrics=${keys}`);
}

function renderCharts() {
  document.querySelectorAll('.chart-box').forEach((box) => {
    drawChart(box, CHART_GROUPS[Number(box.dataset.group)], hist.data);
  });
}

async function pageHistory(main) {
  const first = !main.querySelector('#charts');
  if (first) {
    main.innerHTML = `<div class="page-head"><h2>Istorija</h2>
        <div class="actions">
          <label class="check small"><input type="checkbox" id="band" ${hist.band ? 'checked' : ''}> Min/max opseg</label>
          <div class="seg" id="periods">${HIST_PERIODS.map(([p, label]) => `<button data-p="${p}" class="${p === hist.period ? 'active' : ''}">${label}</button>`).join('')}</div>
        </div></div>
      <div id="hist-note"></div>
      <div id="charts">${CHART_GROUPS.map((g, i) => `<div class="panel"><h3>${esc(g.title)}</h3><div class="chart-box" data-group="${i}"><p class="muted">Učitavanje…</p></div></div>`).join('')}</div>
      <div class="panel"><h3>Izvoz i brisanje</h3>
        <p class="small">Izvoz za izabrani period (${esc(HIST_PERIODS.find((p) => p[0] === hist.period)[1])}): proseci, minimumi i maksimumi svih merenja.</p>
        <div class="actions">
          <a class="btn ghost" id="exp-csv" href="/api/v1/history/export?format=csv&period=${hist.period}" download>Preuzmi CSV</a>
          <a class="btn ghost" id="exp-json" href="/api/v1/history/export?format=json&period=${hist.period}" download>Preuzmi JSON</a>
          <button class="btn danger" id="hist-clear">Obriši istoriju</button>
        </div></div>`;
    main.querySelectorAll('#periods button').forEach((b) => {
      b.onclick = async () => {
        hist.period = b.dataset.p;
        try { localStorage.setItem('histPeriod', hist.period); } catch (e) { /* nije dostupno */ }
        main.innerHTML = '';
        await pageHistory(main);
      };
    });
    $('#band').onchange = (ev) => {
      hist.band = ev.target.checked;
      try { localStorage.setItem('histBand', hist.band ? '1' : '0'); } catch (e) { /* nije dostupno */ }
      renderCharts();
    };
    $('#hist-clear').onclick = async () => {
      if (!confirm('Obrisati celu lokalnu istoriju merenja? Ova radnja se ne može poništiti.')) return;
      try { banner((await adminApi('/history/clear')).message); setTimeout(() => pageHistory(main), 1500); } catch (e) { banner(e.message, true); }
    };
    if (!state.historyResize) {
      state.historyResize = true;
      window.addEventListener('resize', () => { if (state.page === 'istorija' && hist.data) renderCharts(); });
    }
  }
  await loadHistory();
  const note = $('#hist-note');
  if (note) {
    note.innerHTML = hist.data.points.length ? ''
      : `<div class="notice">Još nema sačuvanih podataka za ovaj period. Prvi 5-minutni zapis nastaje 5 minuta posle pokretanja, a satni posle prvog punog sata.${hist.data.time_valid ? '' : ' Vreme još nije sinhronizovano.'}</div>`;
  }
  renderCharts();
}

// ---------------------------------------------------------------------------
// Statistika (specifikacija §5.4)
// ---------------------------------------------------------------------------

const stats = {
  metric: (() => { try { return localStorage.getItem('statsMetric') || 'temperature'; } catch (e) { return 'temperature'; } })(),
  data: null,
  daily: null,
};

// Zajednički tooltip za grafikone statistike: items = [{x, html}], prikazuje se najbliži po x
function attachTooltip(box, svgW, items) {
  const hover = box.querySelector('.hover'), cursor = box.querySelector('.cursor'), tip = box.querySelector('.chart-tip');
  const move = (clientX) => {
    const rect = box.querySelector('svg').getBoundingClientRect();
    const px = clientX - rect.left;
    let best = null;
    items.forEach((it) => { if (best == null || Math.abs(it.x - px) < Math.abs(best.x - px)) best = it; });
    if (!best) return;
    cursor.setAttribute('x1', best.x); cursor.setAttribute('x2', best.x); cursor.setAttribute('visibility', 'visible');
    tip.innerHTML = best.html;
    tip.hidden = false;
    const tipW = tip.offsetWidth;
    tip.style.left = Math.min(Math.max(best.x - tipW / 2, 0), svgW - tipW) + 'px';
  };
  hover.addEventListener('mousemove', (e) => move(e.clientX));
  hover.addEventListener('touchmove', (e) => { move(e.touches[0].clientX); e.preventDefault(); }, { passive: false });
  hover.addEventListener('touchstart', (e) => move(e.touches[0].clientX), { passive: true });
  hover.addEventListener('mouseleave', () => { tip.hidden = true; cursor.setAttribute('visibility', 'hidden'); });
}

// Stubići min-max sa tačkom proseka; bars = [{label, sub, min, max, avg, extra}]
function drawRangeBars(box, bars, meta, color) {
  const width = Math.max(300, box.clientWidth), height = 220;
  const m = { l: 50, r: 14, t: 12, b: 30 };
  const w = width - m.l - m.r, h = height - m.t - m.b;
  const valid = bars.filter((b) => b.avg != null);
  if (!valid.length) {
    box.innerHTML = '<p class="muted">Još nema dovoljno sačuvanih podataka.</p>';
    return;
  }
  let lo = Math.min(...valid.map((b) => b.min)), hi = Math.max(...valid.map((b) => b.max));
  if (hi - lo < 1e-6) { lo -= 1; hi += 1; }
  const pad = (hi - lo) * 0.08; lo -= pad; hi += pad;
  const y = (v) => m.t + h - (v - lo) / (hi - lo) * h;
  const step = w / bars.length;
  const bw = Math.max(3, Math.min(28, step * 0.55));
  const dec = Math.abs(hi - lo) < 10 ? 1 : 0;
  let svg = `<svg width="${width}" height="${height}" class="chart-svg">`;
  for (let i = 0; i <= 4; i++) {
    const yy = m.t + h * i / 4;
    svg += `<line x1="${m.l}" x2="${m.l + w}" y1="${yy}" y2="${yy}" class="grid"/>
      <text x="${m.l - 6}" y="${yy + 3}" text-anchor="end" class="tick">${(hi - (hi - lo) * i / 4).toFixed(dec)}</text>`;
  }
  svg += `<text x="4" y="${m.t + 8}" class="tick unit">${esc(meta.unit)}</text>`;
  const labelEvery = Math.ceil(bars.length / (width < 500 ? 6 : 12));
  const items = [];
  bars.forEach((b, i) => {
    const cx = m.l + step * (i + 0.5);
    if (i % labelEvery === 0) svg += `<text x="${cx}" y="${height - 8}" text-anchor="middle" class="tick">${esc(b.label)}</text>`;
    if (b.avg == null) return;
    svg += `<rect x="${cx - bw / 2}" y="${y(b.max)}" width="${bw}" height="${Math.max(2, y(b.min) - y(b.max))}" rx="3" fill="${color}" opacity="0.35"/>
      <circle cx="${cx}" cy="${y(b.avg)}" r="${Math.min(5, bw / 2 + 1)}" fill="${color}"/>`;
    const d = meta.decimals;
    items.push({ x: cx, html: `<div class="muted">${esc(b.sub || b.label)}</div>
      <div>prosek <b>${niceNum(b.avg, d + 1)} ${esc(meta.unit)}</b></div>
      <div>min ${niceNum(b.min, d)} · max ${niceNum(b.max, d)} ${esc(meta.unit)}</div>${b.extra ? `<div class="muted">${b.extra}</div>` : ''}` });
  });
  svg += `<line class="cursor" x1="0" x2="0" y1="${m.t}" y2="${m.t + h}" visibility="hidden"/>
    <rect class="hover" x="${m.l}" y="${m.t}" width="${w}" height="${h}" fill="transparent"/></svg>`;
  box.innerHTML = `${svg}<div class="chart-tip" hidden></div>`;
  attachTooltip(box, width, items);
}

// Dnevni agregati (po lokalnom datumu uređaja) iz satne istorije za 30 dana
function dailyFromHourly(data) {
  const days = new Map();
  data.points.forEach((p) => {
    if (p[1] == null) return;
    const d = deviceParts(p[0] * 1000);
    const key = `${d.year}-${d.month}-${d.day}`;
    if (!days.has(key)) days.set(key, { label: `${d.day}.${d.month}.`, sub: `${DAYS[d.wd]}, ${d.day}.${d.month}.${d.year}.`, sum: 0, n: 0, min: Infinity, max: -Infinity });
    const g = days.get(key);
    g.sum += p[1]; g.n++;
    g.min = Math.min(g.min, p[2] ?? p[1]);
    g.max = Math.max(g.max, p[3] ?? p[1]);
  });
  return [...days.values()].map((g) => ({ label: g.label, sub: g.sub, min: g.min, max: g.max, avg: g.sum / g.n, extra: `${g.n} satnih zapisa` }));
}

function renderStatsCharts() {
  const meta = stats.data.metrics.find((m) => m.key === stats.metric) || stats.data.metrics[0];
  const pr = [['1h', '1 sat'], ['24h', '24 sata'], ['7d', '7 dana'], ['30d', '30 dana']];
  const bars = pr.map(([p, label]) => {
    const o = meta.periods[p];
    if (!o || !o.samples) return { label, avg: null };
    const trend = { rising: '↗ rast', falling: '↘ pad', stable: '→ stabilno' }[o.trend] || '';
    const change = o.change != null ? `promena ${o.change > 0 ? '+' : ''}${o.change} ${esc(meta.unit)}` : '';
    return { label, sub: `Poslednjih ${label}`, avg: Number(o.avg), min: Number(o.min), max: Number(o.max), extra: [trend, change].filter(Boolean).join(' · ') };
  });
  drawRangeBars($('#stats-periods'), bars, meta, CHART_COLORS[0]);
  if (stats.daily) drawRangeBars($('#stats-daily'), dailyFromHourly(stats.daily), meta, CHART_COLORS[2]);
  else $('#stats-daily').innerHTML = '<p class="muted">Istorija nije dostupna.</p>';
}

async function pageStatistics(main) {
  const s = await api('/statistics');
  stats.data = s;
  if (!s.metrics.some((m) => m.key === stats.metric)) stats.metric = s.metrics[0].key;
  try { stats.daily = await api(`/history?period=30d&metrics=${stats.metric}`); } catch (e) { stats.daily = null; }

  const trendIcon = { rising: '<span title="rast">↗</span>', falling: '<span title="pad">↘</span>', stable: '<span title="stabilno">→</span>' };
  const periods = [['1h', '1 sat'], ['24h', '24 sata'], ['7d', '7 dana'], ['30d', '30 dana']];
  const rows = s.metrics.map((m) => {
    const cells = periods.map(([p]) => {
      const o = m.periods[p];
      if (!o || !o.samples) return '<td class="muted">-</td>';
      const change = o.change != null ? ` <span class="muted small">${o.change > 0 ? '+' : ''}${esc(o.change)}</span>` : '';
      return `<td><b>${esc(o.avg)}</b> ${o.trend ? trendIcon[o.trend] : ''}${change}<br><span class="muted small">${esc(o.min)} - ${esc(o.max)}</span></td>`;
    }).join('');
    return `<tr class="stat-row ${m.key === stats.metric ? 'selected' : ''}" data-key="${m.key}"><td><b>${esc(m.name)}</b><br><span class="muted small">${esc(m.unit)}</span></td>
      <td>${m.current != null ? `<b>${esc(m.current)}</b>` : '<span class="muted">-</span>'}</td>${cells}</tr>`;
  }).join('');
  main.innerHTML = `<div class="page-head"><h2>Statistika</h2>
      <label class="small">Merenje <select id="stats-metric">${s.metrics.map((m) => `<option value="${m.key}" ${m.key === stats.metric ? 'selected' : ''}>${esc(m.name)}</option>`).join('')}</select></label></div>
    <div class="notice">${esc(s.note)}</div>
    <div class="panel"><h3>Poređenje perioda</h3><div class="chart-box" id="stats-periods"></div>
      <p class="muted small">Stubić: minimum - maksimum; tačka: prosek. Pređite mišem ili dodirnite za vrednosti, trend i promenu.</p></div>
    <div class="panel"><h3>Dnevni pregled (30 dana)</h3><div class="chart-box" id="stats-daily"></div></div>
    <div class="panel table-wrap"><h3>Sva merenja</h3>
      <p class="muted small">Ćelija: <b>prosek</b>, trend (↗ rast, ↘ pad, → stabilno) i promena u odnosu na prethodni isti period; ispod: minimum - maksimum. Klik na red bira merenje za grafikone.</p>
      <table><tr><th>Merenje</th><th>Trenutno</th>${periods.map(([, l]) => `<th>${l}</th>`).join('')}</tr>${rows}</table></div>
    <p class="muted small">Sačuvano: ${s.history.records_5min}/${s.history.capacity_5min} zapisa po 5 min,
      ${s.history.records_hour}/${s.history.capacity_hour} satnih zapisa${s.history.pending ? `, ${s.history.pending} čeka sinhronizaciju vremena` : ''}.</p>`;

  const select = async (key) => {
    stats.metric = key;
    try { localStorage.setItem('statsMetric', key); } catch (e) { /* nije dostupno */ }
    await pageStatistics(main);
  };
  $('#stats-metric').onchange = (ev) => select(ev.target.value);
  main.querySelectorAll('.stat-row').forEach((tr) => { tr.onclick = () => select(tr.dataset.key); });
  renderStatsCharts();
}

function pageLater(title) {
  return async (main) => {
    main.innerHTML = `<h2>${esc(title)}</h2><div class="panel muted">Ova stranica biće dostupna u jednoj od narednih faza razvoja.</div>`;
  };
}

const PAGES = {
  dashboard: { render: pageDashboard, refreshMs: 3000 },
  istorija: { render: pageHistory, refreshMs: 60000 },
  statistika: { render: pageStatistics, refreshMs: 60000 },
  senzori: { render: pageSensors, refreshMs: 5000 },
  mqtt: { render: pageMqtt },
  wifi: { render: pageWifi },
  podesavanja: { render: pageSettings },
  alarmi: { render: pageLater('Alarmiranje') },
  sistem: { render: pageSystem, refreshMs: 5000 },
  'o-uredjaju': { render: pageAbout },
};

async function showPage() {
  const name = (location.hash || '#dashboard').slice(1);
  const page = PAGES[name] || PAGES.dashboard;
  state.page = name;
  clearInterval(state.pageTimer);
  document.querySelectorAll('.sidebar a').forEach((a) => a.classList.toggle('active', a.dataset.page === name));
  $('#sidebar').classList.remove('open');
  const main = $('#main');
  const render = async () => {
    try { await page.render(main); } catch (e) { main.innerHTML = `<div class="notice warn">${esc(e.message)}</div>`; }
  };
  await render();
  // Automatsko osvežavanje bez ponovnog učitavanja cele stranice (specifikacija §6.5)
  if (page.refreshMs) {
    state.pageTimer = setInterval(() => { if (state.page === name) render(); }, page.refreshMs);
  }
}

// ---------------------------------------------------------------------------
// Pokretanje
// ---------------------------------------------------------------------------

function initTheme() {
  let theme = null;
  try { theme = localStorage.getItem('theme'); } catch (e) { /* nije dostupno */ }
  if (theme) document.documentElement.dataset.theme = theme;
  $('#theme-btn').onclick = () => {
    const dark = document.documentElement.dataset.theme
      ? document.documentElement.dataset.theme === 'dark'
      : matchMedia('(prefers-color-scheme: dark)').matches;
    const next = dark ? 'light' : 'dark';
    document.documentElement.dataset.theme = next;
    try { localStorage.setItem('theme', next); } catch (e) { /* nije dostupno */ }
  };
}

initTheme();
updateAuthButton();
$('#auth-btn').onclick = async () => {
  if (storedAuth()) { setStoredAuth(null); banner('Odjavljeni ste.'); } else if (await login()) banner('Prijava uspešna.');
};
$('#menu-btn').onclick = () => $('#sidebar').classList.toggle('open');
window.addEventListener('hashchange', showPage);

refreshStatus().then(showPage);
setInterval(refreshStatus, 3000);
setInterval(renderClock, 1000);
