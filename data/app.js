// Meteo stanica -- web interfejs (bez spoljnih biblioteka, radi i bez interneta)
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
  if (seconds == null) return '--';
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

async function pageDashboard(main) {
  const [r, s] = await Promise.all([api('/readings'), api('/status')]);
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
  main.innerHTML = `<h2>Kontrolna tabla</h2><div class="grid">${aqCard}${cards}${statusCard}</div>
    <p class="muted small">Vrednosti su lokalno izmerene; nivo kvaliteta vazduha je lokalna procena po skali EAQI, a ne zvanični indeks.</p>`;
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
      <td>${s.last_reading_age_s != null ? ago(s.last_reading_age_s) : '--'}</td></tr>`).join('');
  const per = d.peripherals.map((p) => `<tr><td><b>${esc(p.name)}</b></td><td>${esc(p.interface)}</td><td>${pinsText(p.pins)}</td>
      <td>${esc(p.i2c_address || '--')}</td><td>${p.supply_v ? p.supply_v + ' V' : '--'}</td><td>${statusBadge(p.status, p.status_text)}</td></tr>`).join('');
  const warn = [];
  if (!d.buses.i2c0_pullups_ok) warn.push('Na glavnoj I2C magistrali (GPIO21/22) nisu pronađeni pull-up otpornici.');
  if (!d.buses.i2c1_pullups_ok) warn.push('Na I2C1 magistrali za SPS30 (GPIO16/17) nisu pronađeni pull-up otpornici -- dodati 4,7–10 kΩ prema 3,3 V.');
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
      <tr><td class="k">Mreža (SSID)</td><td>${esc(w.ssid || '--')}</td></tr>
      <tr><td class="k">IP adresa</td><td>${esc(w.ip || '--')}</td></tr>
      <tr><td class="k">Gateway / maska</td><td>${esc(w.gateway || '--')} / ${esc(w.subnet || '--')}</td></tr>
      <tr><td class="k">DNS</td><td>${esc(w.dns || '--')}</td></tr>
      <tr><td class="k">Signal (RSSI)</td><td>${w.rssi != null ? w.rssi + ' dBm' : '--'}</td></tr>
      <tr><td class="k">Povezan</td><td>${w.connected_for_s != null ? duration(w.connected_for_s) : '--'}</td></tr>
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

async function pageSystem(main) {
  const s = await api('/system');
  main.innerHTML = `<h2>Sistem</h2>
    <div class="panel"><table>
      <tr><td class="k">Uređaj</td><td>${esc(s.device)}</td></tr>
      <tr><td class="k">Verzija firmware-a</td><td>${esc(s.firmware)}</td></tr>
      <tr><td class="k">Čip</td><td>${esc(s.chip)} ${esc(s.chip_revision)}, ${s.cores} jezgra, ${s.cpu_mhz} MHz</td></tr>
      <tr><td class="k">Flash</td><td>${bytes(s.flash_bytes)} · firmware ${bytes(s.sketch_bytes)} od ${bytes(s.ota_partition_bytes)}</td></tr>
      <tr><td class="k">LittleFS</td><td>${bytes(s.fs_used)} / ${bytes(s.fs_total)}</td></tr>
      <tr><td class="k">Slobodna memorija</td><td>${bytes(s.heap_free)} (najmanje ${bytes(s.heap_min_free)}, najveći blok ${bytes(s.heap_max_block)})</td></tr>
      <tr><td class="k">Vreme rada</td><td>${duration(s.uptime_s)}</td></tr>
      <tr><td class="k">Razlog poslednjeg restarta</td><td>${esc(s.reset_reason)}</td></tr>
      <tr><td class="k">Broj pokretanja</td><td>${s.boot_count}</td></tr>
      <tr><td class="k">MAC adresa</td><td>${esc(s.mac)}</td></tr>
      <tr><td class="k">ESP-IDF / Arduino</td><td>${esc(s.sdk)} / ${esc(s.arduino)}</td></tr>
    </table></div>
    <div class="panel"><h3>Akcije</h3><div class="actions">
      <button class="btn ghost" id="portal-btn">Pokreni konfiguracioni portal</button>
      <button class="btn danger" id="restart-btn">Restartuj uređaj</button>
    </div></div>`;
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
      <tr><td class="k">Projekat</td><td>ESP32 R201 -- pametna meteo stanica</td></tr>
      <tr><td class="k">Ploča</td><td>NodeMCU ESP-32S V1.1 (${esc(s.chip)} ${esc(s.chip_revision)})</td></tr>
      <tr><td class="k">Verzija firmware-a</td><td>${esc(s.firmware)}</td></tr>
      <tr><td class="k">Senzori</td><td>BME680, MPU6500, LTR-390, SPS30</td></tr>
      <tr><td class="k">Prikaz</td><td>OLED 0,96" 128×64 SSD1306, statusna LED</td></tr>
    </table></div>`;
}

function pageLater(title) {
  return async (main) => {
    main.innerHTML = `<h2>${esc(title)}</h2><div class="panel muted">Ova stranica biće dostupna u jednoj od narednih faza razvoja.</div>`;
  };
}

const PAGES = {
  dashboard: { render: pageDashboard, refreshMs: 3000 },
  istorija: { render: pageLater('Istorija') },
  statistika: { render: pageLater('Statistika') },
  senzori: { render: pageSensors, refreshMs: 5000 },
  mqtt: { render: pageLater('MQTT') },
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
