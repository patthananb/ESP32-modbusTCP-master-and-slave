#pragma once
#include <pgmspace.h>

/*
 * Single-page web UI for the Modbus TCP slave.
 * Plain HTML + vanilla JS, no build step, no external CDN.
 * Talks to /api/config (GET/POST) and /api/live (GET).
 */
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 Modbus Slave</title>
<style>
  :root { color-scheme: light dark; }
  body { font-family: -apple-system, system-ui, sans-serif; max-width: 980px;
         margin: 1.5rem auto; padding: 0 1rem; }
  h1 { margin-bottom: 0.2rem; }
  .sub { color: #888; margin-top: 0; }
  table { width: 100%; border-collapse: collapse; margin-top: 0.8rem; }
  th, td { border: 1px solid #8884; padding: 0.35rem 0.5rem; text-align: left;
           font-size: 0.9rem; vertical-align: middle; }
  th { background: #8881; }
  input[type=number], input[type=text], select { width: 100%; box-sizing: border-box;
           padding: 0.2rem 0.3rem; font: inherit; }
  input[type=number] { max-width: 6rem; }
  td.live { font-variant-numeric: tabular-nums; min-width: 6rem; }
  td.act  { width: 2.4rem; text-align: center; }
  button { padding: 0.4rem 0.9rem; font: inherit; cursor: pointer; }
  .row { display: flex; gap: 0.5rem; margin-top: 0.8rem; flex-wrap: wrap; }
  .ok  { color: #2a8; }
  .err { color: #c33; }
  small { color: #888; }
</style>
</head>
<body>
<h1>ESP32-S3 Modbus TCP Slave</h1>
<p class="sub">Configurable holding-register map. Changes save to flash and reboot the device.</p>

<div id="status"></div>

<table id="tbl">
  <thead>
    <tr>
      <th>HREG</th>
      <th>Source</th>
      <th>Type</th>
      <th>Scale</th>
      <th>Description</th>
      <th>Live</th>
      <th class="act"></th>
    </tr>
  </thead>
  <tbody></tbody>
</table>

<div class="row">
  <button id="add">+ Add register</button>
  <button id="save">Save &amp; reboot</button>
  <button id="reset">Reset to defaults</button>
</div>
<p><small>32-bit types (uint32/int32/float32) occupy two consecutive registers
(big-endian word order). Scale is applied to the source value before packing.</small></p>

<script>
let SOURCES = [], TYPES = [];
let rows = [];

async function load() {
  const r = await fetch('/api/config');
  const j = await r.json();
  SOURCES = j.sources;
  TYPES = j.types;
  rows = j.registers;
  render();
  poll();
}

function render() {
  const tb = document.querySelector('#tbl tbody');
  tb.innerHTML = '';
  rows.forEach((r, i) => {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td><input type=number min=0 max=999 value="${r.address}" data-k=address></td>
      <td><select data-k=source>${SOURCES.map((s, idx) =>
            `<option value=${idx} ${idx==r.source?'selected':''}>${s}</option>`).join('')}</select></td>
      <td><select data-k=type>${TYPES.map((t, idx) =>
            `<option value=${idx} ${idx==r.type?'selected':''}>${t}</option>`).join('')}</select></td>
      <td><input type=number step=any value="${r.scale}" data-k=scale></td>
      <td><input type=text maxlength=47 value="${r.description.replace(/"/g,'&quot;')}" data-k=description></td>
      <td class=live data-addr="${r.address}">…</td>
      <td class=act><button data-i=${i}>×</button></td>`;
    tr.querySelectorAll('[data-k]').forEach(el => {
      el.addEventListener('change', () => {
        const k = el.dataset.k;
        rows[i][k] = (el.type === 'number') ? Number(el.value) :
                     (k === 'source' || k === 'type') ? Number(el.value) : el.value;
      });
    });
    tr.querySelector('button').addEventListener('click', () => {
      rows.splice(i, 1); render();
    });
    tb.appendChild(tr);
  });
}

async function poll() {
  try {
    const r = await fetch('/api/live');
    const j = await r.json();
    document.querySelectorAll('td.live').forEach(td => {
      const a = Number(td.dataset.addr);
      const v = j.registers[a];
      td.textContent = (v === undefined) ? '—' : v;
    });
  } catch (e) {}
  setTimeout(poll, 1000);
}

document.getElementById('add').onclick = () => {
  const nextAddr = rows.length ? Math.max(...rows.map(r => r.address)) + 1 : 0;
  rows.push({address: nextAddr, source: 0, type: 1, scale: 10,
             description: ''});
  render();
};

document.getElementById('save').onclick = async () => {
  const s = document.getElementById('status');
  s.textContent = 'Saving...';
  s.className = '';
  const r = await fetch('/api/config', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({registers: rows})
  });
  if (r.ok) {
    s.textContent = 'Saved. Rebooting...';
    s.className = 'ok';
  } else {
    const t = await r.text();
    s.textContent = 'Error: ' + t;
    s.className = 'err';
  }
};

document.getElementById('reset').onclick = async () => {
  if (!confirm('Reset all registers to factory defaults?')) return;
  const r = await fetch('/api/config/reset', {method: 'POST'});
  if (r.ok) location.reload();
};

load();
</script>
</body>
</html>
)HTML";
