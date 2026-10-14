/*
 * AFC lanes page for Snapmaker Orca.
 *
 *   index.html?view=panel  lane panel beside the U1 device page
 *   index.html?view=print  lane mapping step before a print
 *
 * Talks to the slicer over the same bridge as the device page
 * (window.wx.postMessage -> SSWCP, answers arrive as window "message"
 * events). Every printer change is built in C++ from the fields sent here.
 */
(function () {
  'use strict';

  const params = new URLSearchParams(location.search);
  const VIEW = params.get('view') === 'print' ? 'print' : 'panel';
  const darkParam = params.get('dark_mode');
  if (darkParam === '1' || darkParam === 'true' ||
      (darkParam === null && window.matchMedia && matchMedia('(prefers-color-scheme: dark)').matches)) {
    document.documentElement.classList.add('dark');
  }

  // ------------------------------------------------------------------ bridge
  let seq = 0;
  const pending = new Map();

  function call(cmd, args) {
    return new Promise((resolve, reject) => {
      const id = ++seq;
      pending.set(id, { resolve, reject });
      const msg = JSON.stringify({ header: { afc_id: id }, payload: { cmd: cmd, params: args || {} } });
      try {
        window.wx.postMessage(msg);
      } catch (e) {
        pending.delete(id);
        reject(new Error('The slicer bridge is not available'));
        return;
      }
      setTimeout(() => {
        if (pending.has(id)) {
          pending.delete(id);
          reject(new Error('The slicer did not answer'));
        }
      }, 30000);
    });
  }

  window.addEventListener('message', (event) => {
    let data = event.data;
    if (typeof data === 'string') {
      try { data = JSON.parse(data); } catch (e) { return; }
    }
    const id = data && data.header && data.header.afc_id;
    if (!id || !pending.has(id)) return;
    const p = pending.get(id);
    pending.delete(id);
    const payload = data.payload || {};
    if (payload.code === 200) p.resolve(payload.data);
    else p.reject(new Error(payload.message || 'failed'));
  });

  // ------------------------------------------------------------------ helpers
  const $app = document.getElementById('app');
  const $toast = document.getElementById('toast');

  function esc(v) {
    return String(v == null ? '' : v).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  let toastTimer = null;
  function toast(text, bad) {
    $toast.textContent = text;
    $toast.className = 'toast show' + (bad ? ' bad' : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { $toast.className = 'toast'; }, bad ? 5000 : 2600);
  }

  function hexOk(c) { return /^#[0-9A-Fa-f]{6}$/.test(c || ''); }

  function spoolStyle(lane) {
    const colors = (lane && lane.multi_colors && lane.multi_colors.length > 1) ? lane.multi_colors.filter(hexOk) : [];
    if (colors.length > 1) {
      const step = 360 / colors.length;
      return 'background: conic-gradient(' + colors.map((c, i) => c + ' ' + (i * step) + 'deg ' + ((i + 1) * step) + 'deg').join(', ') + ')';
    }
    const c = lane && hexOk(lane.color) ? lane.color : null;
    return c ? 'background:' + c : '';
  }

  function spool(lane, small) {
    const has = lane && lane.has_filament && (hexOk(lane.color) || (lane.multi_colors || []).length);
    return '<span class="spool' + (small ? ' sm' : '') + (has ? '' : ' none') + '" style="' + (has ? spoolStyle(lane) : '') + '"></span>';
  }

  function swatch(color, small) {
    return '<span class="spool' + (small ? ' sm' : '') + (hexOk(color) ? '' : ' none') + '" style="' + (hexOk(color) ? 'background:' + color : '') + '"></span>';
  }

  function headName(idx) { return idx >= 0 ? 'Head ' + (idx + 1) : 'Toolhead'; }

  const UNIT_ICONS = [
    [/bambu|ams/i, 'AMS'], [/box.?turtle/i, 'BT'], [/night.?owl/i, 'NO'], [/openams|oams/i, 'OA'],
    [/ace/i, 'ACE'], [/htlf/i, 'HT'], [/quattro/i, 'QB'], [/emu/i, 'EMU'], [/vivid/i, 'VV'], [/claymore/i, 'CM'],
  ];
  function unitIcon(type, name) {
    const s = (type || '') + ' ' + (name || '');
    for (const [re, label] of UNIT_ICONS) if (re.test(s)) return label;
    return (name || '?').slice(0, 2).toUpperCase();
  }
  function prettyType(type) { return (type || 'Unit').replace(/_/g, ' '); }

  function materialKey(m) {
    return String(m || '').toUpperCase().split(/[ \-_]/)[0].replace(/\+$/, '');
  }

  function rgb(c) {
    if (!hexOk(c)) return null;
    return [parseInt(c.substr(1, 2), 16), parseInt(c.substr(3, 2), 16), parseInt(c.substr(5, 2), 16)];
  }
  function colorDistance(a, b) {
    const x = rgb(a), y = rgb(b);
    if (!x || !y) return 0;
    return Math.sqrt((x[0] - y[0]) ** 2 + (x[1] - y[1]) ** 2 + (x[2] - y[2]) ** 2);
  }

  function laneSubtitle(lane) {
    if (!lane.has_filament) return 'Empty';
    const parts = [lane.material || 'Unknown material'];
    if (lane.vendor) parts.push(lane.vendor);
    return parts.join(' · ');
  }

  function grams(v) { return Math.round(v) + ' g'; }

  // ------------------------------------------------------------------ state
  let state = null;          // last answer from sw_AFC_GetState
  let selected = null;       // lane name open in the editor (panel)
  let editorDirty = false;
  let busy = false;
  let mapping = null;        // print view: filament id -> lane name
  let openMenu = null;       // print view: filament id with its picker open

  function lanes() { return (state && state.afc && state.afc.lanes) || []; }
  function laneByName(name) { return lanes().find((l) => l.name === name); }

  async function load(refresh) {
    try {
      const data = await call('sw_AFC_GetState', { refresh: refresh !== false, project: VIEW === 'print' });
      state = data;
      if (data && data.error) toast(data.error, true);
      if (VIEW === 'print' && mapping === null) resetMapping();
      render();
    } catch (e) {
      if (!state) renderError(e.message);
      else toast(e.message, true);
    }
  }

  function render() {
    if (VIEW === 'print') renderPrint();
    else renderPanel();
  }

  function renderError(message) {
    $app.innerHTML = '<div class="empty-state"><div class="big">Could not read AFC</div>' + esc(message) +
      '<div style="margin-top:14px"><button class="btn" id="retry">Try again</button></div></div>';
    document.getElementById('retry').onclick = () => load(true);
  }

  // ================================================================== panel
  function renderPanel() {
    const afc = state && state.afc;
    if (!state || !state.connected) {
      $app.innerHTML = bar('', '') + '<div class="empty-state"><div class="big">No printer connected</div>Connect your printer on the device page to see its AFC units.</div>';
      bindBar();
      return;
    }
    if (!afc || !afc.present || !afc.lanes.length) {
      $app.innerHTML = bar('', '') + '<div class="empty-state"><div class="big">No AFC on this printer</div>When AFC is installed, its units and lanes show up here.</div>';
      bindBar();
      return;
    }

    const loadedCount = afc.lanes.filter((l) => l.has_filament).length;
    const sub = 'AFC ' + esc(afc.version || '') + ' · ' + afc.units.length + ' unit' + (afc.units.length === 1 ? '' : 's') +
      ' · ' + loadedCount + '/' + afc.lanes.length + ' lanes loaded';

    let html = bar(sub, '<button class="btn" id="sync" title="Use the AFC lanes as the project filaments">Sync to slicer</button>');
    html += '<div class="scroll" id="scroll">' + headsStrip(afc);
    for (const unit of afc.units) html += unitCard(unit);
    html += '</div>';
    if (selected && laneByName(selected)) html += editor(laneByName(selected));
    $app.innerHTML = html;
    bindBar();
    const sync = document.getElementById('sync');
    if (sync) sync.onclick = () => call('sw_AFC_SyncToSlicer').catch((e) => toast(e.message, true));
    for (const el of document.querySelectorAll('.lane[data-lane]')) {
      el.onclick = () => { selected = el.dataset.lane === selected ? null : el.dataset.lane; editorDirty = false; render(); };
    }
    if (selected) bindEditor(laneByName(selected));
  }

  function bar(sub, extra) {
    return '<div class="bar"><div><h1>AFC lanes</h1>' + (sub ? '<div class="sub">' + sub + '</div>' : '') + '</div>' +
      '<div class="grow"></div>' + extra +
      '<button class="btn icon ghost" id="refresh" title="Read AFC again">⟳</button>' +
      (VIEW === 'panel' ? '<button class="btn icon ghost" id="hide" title="Hide the AFC panel">✕</button>' : '') + '</div>';
  }

  function bindBar() {
    const r = document.getElementById('refresh');
    if (r) r.onclick = () => load(true);
    const h = document.getElementById('hide');
    if (h) h.onclick = () => call('sw_AFC_SetPanelVisible', { visible: false });
  }

  function headsStrip(afc) {
    const heads = new Map();
    for (const name of afc.extruders || []) {
      const m = /^extruder(\d*)$/.exec(name);
      heads.set(m ? (m[1] ? +m[1] : 0) : heads.size, null);
    }
    for (const l of afc.lanes) {
      if (l.extruder_index >= 0 && !heads.has(l.extruder_index)) heads.set(l.extruder_index, null);
      if (l.tool_loaded && l.extruder_index >= 0) heads.set(l.extruder_index, l);
    }
    if (heads.size < 2 && !afc.lanes.some((l) => l.tool_loaded)) return '';
    let html = '<div class="heads">';
    for (const [idx, lane] of [...heads.entries()].sort((a, b) => a[0] - b[0])) {
      html += '<div class="head">' + spool(lane, true) + '<div style="min-width:0"><div class="label">' + headName(idx) + '</div>' +
        '<div class="what">' + (lane ? esc(lane.name) + ' · ' + esc(lane.material || '') : '<span class="hint">Empty</span>') + '</div></div></div>';
    }
    return html + '</div>';
  }

  function unitCard(unit) {
    let html = '<div class="unit"><div class="unit-head"><div class="unit-icon">' + esc(unitIcon(unit.type, unit.name)) + '</div>' +
      '<div><div class="name">' + esc(unit.name.replace(/_/g, ' ')) + '</div><div class="type">' + esc(prettyType(unit.type)) + '</div></div></div><div class="lanes">';
    for (const name of unit.lanes) {
      const l = laneByName(name);
      if (l) html += laneTile(l);
    }
    return html + '</div></div>';
  }

  function laneTile(l) {
    const maps = (l.maps || []).length ? l.maps.map((m) => '<span class="tchip">' + esc(m) + '</span>').join('') : '<span class="tchip none">no T</span>';
    const initial = l.initial_weight > 0 ? l.initial_weight : 1000;
    const pct = Math.max(0, Math.min(100, (l.weight / initial) * 100));
    const meterCls = pct < 5 ? ' out' : pct < 15 ? ' low' : '';
    let status = '';
    if (l.tool_loaded) status = '<span class="pill ok"><span class="dot"></span>In ' + headName(l.extruder_index).toLowerCase() + '</span>';
    if (l.spool_id) status += '<span class="pill accent" title="Spoolman spool">#' + esc(l.spool_id) + '</span>';
    return '<div class="lane' + (l.has_filament ? '' : ' empty') + (l.tool_loaded ? ' in-tool' : '') + (l.name === selected ? ' selected' : '') + '" data-lane="' + esc(l.name) + '">' +
      '<div class="lane-top">' + spool(l) + '<div style="min-width:0"><div class="lane-name">' + esc(l.name) + '</div>' +
      '<div class="lane-meta">' + maps + '</div></div></div>' +
      '<div class="lane-sub" title="' + esc(l.filament_name || '') + '">' + esc(laneSubtitle(l)) + '</div>' +
      (l.has_filament && l.weight > 0 ? '<div class="meter' + meterCls + '"><i style="width:' + pct.toFixed(0) + '%"></i></div><div class="grams"><span>' + grams(l.weight) + '</span><span>' + (l.extruder_index >= 0 ? headName(l.extruder_index) : '') + '</span></div>' : '') +
      (status ? '<div class="lane-meta">' + status + '</div>' : '') +
      '</div>';
  }

  const MATERIALS = ['PLA', 'PLA+', 'PLA-CF', 'PETG', 'PETG-CF', 'ABS', 'ASA', 'TPU', 'PA', 'PA-CF', 'PC', 'PVA', 'HIPS', 'PET', 'PPS'];

  function editor(l) {
    const maxTool = Math.max(state.afc.max_tool, (l.maps || []).reduce((a, m) => Math.max(a, +m.slice(1) || 0), 0));
    let tOptions = '';
    for (let t = 0; t <= maxTool; t++) {
      const owner = state.afc.lanes.find((x) => (x.maps || []).includes('T' + t));
      tOptions += '<option value="T' + t + '"' + ((l.maps || [])[0] === 'T' + t ? ' selected' : '') + '>T' + t + (owner && owner.name !== l.name ? ' (now ' + esc(owner.name) + ')' : '') + '</option>';
    }
    const color = hexOk(l.color) ? l.color : '#FFFFFF';
    return '<div class="drawer" id="drawer"><h2>' + spool(l, true) + esc(l.name) + '<span class="hint">' + esc(l.unit.replace(/_/g, ' ')) + (l.extruder_index >= 0 ? ' · feeds ' + headName(l.extruder_index).toLowerCase() : '') + '</span></h2>' +
      '<div class="form">' +
      '<div class="field"><label>Colour</label><div class="color-row"><input type="color" id="f-colorpick" value="' + esc(color) + '"><input id="f-color" value="' + esc(color) + '" maxlength="7"></div></div>' +
      '<div class="field"><label>Material</label><input id="f-material" list="materials" value="' + esc(l.material || '') + '"><datalist id="materials">' + MATERIALS.map((m) => '<option value="' + m + '">').join('') + '</datalist></div>' +
      '<div class="field"><label>Remaining (g)</label><input id="f-weight" type="number" min="0" step="1" value="' + esc(Math.round(l.weight || 0)) + '"></div>' +
      '<div class="field"><label>Tool (T number)</label><select id="f-map">' + (l.maps && l.maps.length ? '' : '<option value="">none</option>') + tOptions + '</select></div>' +
      '<div class="field"><label>Nozzle °C</label><input id="f-nozzle" type="number" min="0" max="500" value="' + esc(l.nozzle_temp || '') + '"></div>' +
      '<div class="field"><label>Bed °C</label><input id="f-bed" type="number" min="0" max="200" value="' + esc(l.bed_temp || '') + '"></div>' +
      '<div class="field wide"><label>Spoolman spool ID' + (state.afc.spoolman ? '' : ' (Spoolman is not set up in AFC)') + '</label>' +
      '<div class="color-row"><input id="f-spool" type="number" min="0" step="1" value="' + esc(l.spool_id || '') + '" placeholder="none">' +
      (state.afc.spoolman_url ? '<button class="btn" id="open-spool"' + (l.spool_id ? '' : ' disabled') + '>Open in Spoolman</button>' : '') + '</div>' +
      '<span class="hint">Setting a spool ID makes AFC fill colour, material and weight from Spoolman.</span></div>' +
      '</div>' +
      '<div class="actions"><button class="btn primary" id="save" disabled>Save</button><button class="btn ghost" id="close">Close</button><div class="grow"></div>' +
      '<button class="btn" id="act-load"' + (l.has_filament && !l.tool_loaded ? '' : ' disabled') + ' title="CHANGE_TOOL: swap this lane into its toolhead">Load</button>' +
      '<button class="btn" id="act-unload"' + (l.tool_loaded ? '' : ' disabled') + ' title="TOOL_UNLOAD: pull it out of the toolhead">Unload</button>' +
      '<button class="btn danger" id="act-eject"' + (l.has_filament && !l.tool_loaded ? '' : ' disabled') + ' title="LANE_UNLOAD: retract it out of the unit">Eject</button></div></div>';
  }

  function bindEditor(l) {
    const $ = (id) => document.getElementById(id);
    const save = $('save');
    const dirty = () => { editorDirty = true; save.disabled = false; };
    for (const id of ['f-color', 'f-material', 'f-weight', 'f-map', 'f-nozzle', 'f-bed', 'f-spool']) {
      $(id).addEventListener('input', dirty);
      $(id).addEventListener('change', dirty);
    }
    $('f-colorpick').addEventListener('input', () => { $('f-color').value = $('f-colorpick').value.toUpperCase(); dirty(); });
    $('f-color').addEventListener('input', () => { if (hexOk($('f-color').value)) $('f-colorpick').value = $('f-color').value; });
    $('close').onclick = () => { selected = null; editorDirty = false; render(); };
    const open = $('open-spool');
    if (open) open.onclick = () => call('sw_AFC_OpenSpoolman', { spool_id: l.spool_id }).catch((e) => toast(e.message, true));

    save.onclick = async () => {
      const changes = {};
      const spoolVal = $('f-spool').value.trim();
      const spoolId = spoolVal === '' ? 0 : parseInt(spoolVal, 10);
      if (spoolId !== (l.spool_id || 0)) {
        // Spoolman owns the rest once a spool is linked; send the ID alone.
        changes.spool_id = spoolId;
      } else {
        const color = $('f-color').value.trim().toUpperCase();
        if (!hexOk(color)) return toast('Colour must look like #RRGGBB', true);
        if (color !== (l.color || '').toUpperCase()) changes.color = color;
        const material = $('f-material').value.trim();
        if (material && material !== l.material) changes.material = material;
        const weight = parseFloat($('f-weight').value);
        if (!isNaN(weight) && Math.round(weight) !== Math.round(l.weight || 0)) changes.weight = weight;
        const map = $('f-map').value;
        if (map && map !== (l.maps || [])[0]) changes.map = map;
        const nozzle = parseInt($('f-nozzle').value, 10);
        if (!isNaN(nozzle) && nozzle !== l.nozzle_temp) changes.nozzle_temp = nozzle;
        const bed = parseInt($('f-bed').value, 10);
        if (!isNaN(bed) && bed !== l.bed_temp) changes.bed_temp = bed;
      }
      if (!Object.keys(changes).length) { editorDirty = false; save.disabled = true; return; }
      await run(() => call('sw_AFC_EditLane', { lane: l.name, changes: changes }), changes.spool_id !== undefined ? 'Spool linked; AFC is pulling it from Spoolman' : 'Saved to AFC');
      editorDirty = false;
    };

    const action = (name, question) => async () => {
      if (!confirm(question)) return;
      await run(() => call('sw_AFC_LaneAction', { lane: l.name, action: name }), 'Sent to AFC');
    };
    $('act-load').onclick = action('load', 'Load ' + l.name + ' into ' + headName(l.extruder_index).toLowerCase() + '? AFC unloads whatever is in that toolhead first.');
    $('act-unload').onclick = action('unload', 'Unload ' + l.name + ' from its toolhead?');
    $('act-eject').onclick = action('eject', 'Eject ' + l.name + ' out of the unit?');
  }

  async function run(fn, okText) {
    if (busy) return;
    busy = true;
    $app.classList.add('busy');
    try {
      const data = await fn();
      if (data && data.afc) state = data.project ? data : Object.assign({}, state, { afc: data.afc, connected: data.connected });
      if (data && data.error) toast(data.error, true);
      else if (okText) toast(okText);
    } catch (e) {
      toast(e.message, true);
    } finally {
      busy = false;
      $app.classList.remove('busy');
      render();
    }
  }

  // ================================================================== print
  function filaments() { return (state && state.project && state.project.filaments) || []; }

  function resetMapping() {
    mapping = {};
    const sug = (state && state.project && state.project.suggestion) || [];
    filaments().forEach((f, i) => { if (f.used) mapping[f.id] = sug[i] || ''; });
  }

  function rowWarnings(f) {
    const out = [];
    const laneName = mapping[f.id];
    if (!laneName) return [['bad', 'Pick a lane']];
    const l = laneByName(laneName);
    if (!l) return [['bad', 'Lane is gone']];
    if (!l.has_filament) out.push(['bad', laneName + ' is empty']);
    const dup = filaments().filter((o) => o.used && o.id !== f.id && mapping[o.id] === laneName);
    if (dup.length && !state.afc.multiple_mapping) out.push(['bad', 'Also picked for ' + dup.map((o) => 'T' + o.id).join(', ')]);
    if (l.material && f.type && materialKey(l.material) !== materialKey(f.type)) out.push(['warn', 'Lane has ' + l.material + ', the plate uses ' + f.type]);
    if (f.grams > 0 && l.weight > 0 && l.weight < f.grams) out.push(['warn', 'Needs ' + grams(f.grams) + ', about ' + grams(l.weight) + ' left']);
    if (l.has_filament && colorDistance(l.color, f.color) > 120) out.push(['', 'Different colour']);
    return out;
  }

  function renderPrint() {
    const afc = state && state.afc;
    if (!afc || !afc.present || !afc.lanes.length) {
      $app.innerHTML = '<div class="empty-state"><div class="big">AFC is not answering</div>Continue to print with the lanes AFC has mapped now.</div>' +
        '<div class="footer"><div class="grow"></div><button class="btn" id="cancel">Cancel</button><button class="btn primary" id="skip">Continue</button></div>';
      document.getElementById('cancel').onclick = () => finish('cancel');
      document.getElementById('skip').onclick = () => finish('continue');
      return;
    }
    const fils = filaments();
    const used = fils.filter((f) => f.used);
    let blocking = false;
    let rows = '';
    for (const f of [...used, ...fils.filter((x) => !x.used)]) {
      const warns = f.used ? rowWarnings(f) : [];
      if (warns.some((w) => w[0] === 'bad')) blocking = true;
      const l = laneByName(mapping[f.id]);
      rows += '<div class="map-row' + (f.used ? '' : ' unused') + '">' +
        '<div class="fil">' + swatch(f.color) + '<div style="min-width:0"><div class="t">T' + f.id + ' · ' + esc(f.type || '?') + '</div>' +
        '<div class="s" title="' + esc(f.preset) + '">' + (f.used ? (f.grams > 0 ? grams(f.grams) + ' · ' : '') + esc(f.preset || '') : 'Not used on this plate') + '</div></div></div>' +
        '<div class="arrow">→</div>' +
        (f.used ? picker(f, l) : '<div class="hint">—</div>') +
        '<div class="warns">' + warns.map((w) => '<span class="pill ' + w[0] + '">' + esc(w[1]) + '</span>').join('') + '</div></div>';
    }

    $app.innerHTML =
      '<div class="bar"><div><h1>Match filaments to AFC lanes</h1><div class="sub">' + used.length + ' filament' + (used.length === 1 ? '' : 's') + ' on this plate · ' +
      afc.lanes.filter((x) => x.has_filament).length + ' loaded lanes in ' + afc.units.length + ' unit' + (afc.units.length === 1 ? '' : 's') + '</div></div>' +
      '<div class="grow"></div><button class="btn" id="auto" title="Match by material, then colour">Auto match</button>' +
      '<button class="btn icon ghost" id="refresh" title="Read AFC again">⟳</button></div>' +
      '<div class="scroll" id="scroll">' + summary(used) +
      (state.project.sliced ? '' : '<div class="summary"><span class="pill warn">Slice the plate to see grams and which filaments it uses</span></div>') +
      '<div class="map-head"><div>Plate filament</div><div></div><div>AFC lane</div></div>' + rows + '</div>' +
      '<div class="footer"><span class="hint">Applying sets each T number to its lane with SET_MAP.</span><div class="grow"></div>' +
      '<button class="btn" id="cancel">Cancel</button>' +
      '<button class="btn" id="skip" title="Print with the T map AFC has now">Keep current map</button>' +
      '<button class="btn primary" id="apply"' + (blocking ? ' disabled' : '') + '>Apply and continue</button></div>';

    document.getElementById('auto').onclick = () => { resetMapping(); render(); };
    document.getElementById('refresh').onclick = () => load(true);
    document.getElementById('cancel').onclick = () => finish('cancel');
    document.getElementById('skip').onclick = () => finish('continue');
    document.getElementById('apply').onclick = apply;
    for (const el of document.querySelectorAll('.pick-btn')) {
      el.onclick = (e) => { e.stopPropagation(); const id = +el.dataset.fil; openMenu = openMenu === id ? null : id; render(); };
    }
    for (const el of document.querySelectorAll('.opt[data-lane]')) {
      el.onclick = (e) => {
        e.stopPropagation();
        if (el.classList.contains('disabled')) return;
        mapping[+el.dataset.fil] = el.dataset.lane;
        openMenu = null;
        render();
      };
    }
  }

  function picker(f, l) {
    let html = '<div class="pick"><button class="pick-btn" data-fil="' + f.id + '">' + spool(l) +
      '<div class="txt"><div class="l1">' + (l ? esc(l.name) + ' · ' + esc(l.material || '?') : 'Pick a lane') + '</div>' +
      '<div class="l2">' + (l ? esc(l.unit.replace(/_/g, ' ')) + (l.weight > 0 ? ' · ' + grams(l.weight) + ' left' : '') + (l.extruder_index >= 0 ? ' · ' + headName(l.extruder_index) : '') : '') + '</div></div>' +
      '<span class="caret">▾</span></button>';
    if (openMenu === f.id) {
      html += '<div class="menu">';
      for (const unit of state.afc.units) {
        html += '<div class="grp">' + esc(unit.name.replace(/_/g, ' ')) + ' · ' + esc(prettyType(unit.type)) + '</div>';
        for (const name of unit.lanes) {
          const x = laneByName(name);
          if (!x) continue;
          const takenBy = filaments().filter((o) => o.used && o.id !== f.id && mapping[o.id] === name).map((o) => 'T' + o.id);
          const sameMat = x.material && f.type && materialKey(x.material) === materialKey(f.type);
          html += '<div class="opt' + (x.has_filament ? '' : ' disabled') + (mapping[f.id] === name ? ' cur' : '') + '" data-fil="' + f.id + '" data-lane="' + esc(name) + '">' +
            spool(x, true) + '<div class="txt"><div class="l1">' + esc(name) + ' · ' + esc(x.has_filament ? (x.material || '?') : 'empty') +
            (sameMat ? '' : '') + '</div><div class="l2">' + esc(x.vendor || '') + (x.weight > 0 && x.has_filament ? ' · ' + grams(x.weight) : '') +
            (x.extruder_index >= 0 ? ' · ' + headName(x.extruder_index) : '') + (takenBy.length ? ' · used by ' + takenBy.join(', ') : '') + '</div></div>' +
            (x.maps || []).map((m) => '<span class="tchip">' + esc(m) + '</span>').join('') + '</div>';
        }
      }
      html += '</div>';
    }
    return html + '</div>';
  }

  // Toolheads that will swap filament mid-print: more than one picked lane feeds them.
  function summary(used) {
    const perHead = new Map();
    for (const f of used) {
      const l = laneByName(mapping[f.id]);
      if (!l || l.extruder_index < 0) continue;
      if (!perHead.has(l.extruder_index)) perHead.set(l.extruder_index, new Set());
      perHead.get(l.extruder_index).add(l.name);
    }
    const units = new Set(used.map((f) => (laneByName(mapping[f.id]) || {}).unit).filter(Boolean));
    let chips = '<span class="pill accent">' + units.size + ' unit' + (units.size === 1 ? '' : 's') + ' in use</span>';
    for (const [idx, set] of [...perHead.entries()].sort((a, b) => a[0] - b[0])) {
      if (set.size > 1) chips += '<span class="pill warn" title="AFC swaps filament in this toolhead during the print">' + headName(idx) + ' swaps between ' + esc([...set].join(', ')) + '</span>';
      else chips += '<span class="pill ok">' + headName(idx) + ': ' + esc([...set][0]) + '</span>';
    }
    return '<div class="summary">' + chips + '</div>';
  }

  async function apply() {
    const pairs = filaments().filter((f) => f.used).map((f) => ({ tool: f.id, lane: mapping[f.id] }));
    if (busy) return;
    busy = true;
    $app.classList.add('busy');
    try {
      const data = await call('sw_AFC_ApplyMapping', { mapping: pairs });
      if (data && data.error) {
        toast(data.error, true);
        return;
      }
      await finish('continue');
    } catch (e) {
      toast(e.message, true);
    } finally {
      busy = false;
      $app.classList.remove('busy');
    }
  }

  function finish(action) {
    return call('sw_AFC_FinishPrintMapping', { action: action }).catch((e) => toast(e.message, true));
  }

  document.addEventListener('click', () => {
    if (openMenu !== null) { openMenu = null; render(); }
  });

  // ------------------------------------------------------------------ start
  // The webview may attach window.wx a moment after the document starts.
  (function start(tries) {
    if (window.wx || tries <= 0) return load(true);
    setTimeout(() => start(tries - 1), 100);
  })(30);
  if (VIEW === 'panel') {
    setInterval(() => {
      if (document.hidden || busy || editorDirty || pending.size) return;
      load(true);
    }, 5000);
  }
})();
