/*
 * Stand-in for the slicer bridge when the page is opened in a plain browser
 * with ?mock=1 and no window.wx: a U1 with a Box Turtle and a Bambu AMS 2 behind BridgeBox.
 * Used to work on the page and to screenshot it; inert inside the slicer.
 */
(function () {
  'use strict';
  if (window.wx || !/[?&]mock=1/.test(location.search)) return;

  function lane(name, unit, index, maps, head, color, material, vendor, weight, extra) {
    return Object.assign({
      name: name, unit: unit, unit_type: '', index: index, maps: maps, extruder: head ? 'extruder' + head : 'extruder',
      extruder_index: head, color: color, multi_colors: [], material: material, sub_type: '', vendor: vendor,
      filament_name: vendor ? vendor + ' ' + material : '', spool_id: 0, weight: weight, initial_weight: 1000,
      density: 1.24, nozzle_temp: material === 'PETG' ? 240 : 215, bed_temp: material === 'PETG' ? 70 : 60,
      prep: !!material, load: !!material, tool_loaded: false, has_filament: !!material, status: material ? 'Ready' : 'None', td: '',
    }, extra || {});
  }

  const afc = {
    present: true, version: '1.0.30', spoolman: true, spoolman_url: 'http://spoolman.local:7912', multiple_mapping: false,
    current_lane: 'lane1', extruders: ['extruder', 'extruder1', 'extruder2', 'extruder3'],
    units: [
      { name: 'Turtle_1', type: 'Box_Turtle', lanes: ['lane1', 'lane2', 'lane3', 'lane4'] },
      { name: 'Bambu_AMS_1', type: 'BambuAMS', lanes: ['lane5', 'lane6', 'lane7', 'lane8'] },
    ],
    lanes: [
      lane('lane1', 'Turtle_1', 1, ['T0'], 0, '#E8E8E8', 'PLA', 'Polymaker', 812, { tool_loaded: true, spool_id: 41 }),
      lane('lane2', 'Turtle_1', 2, ['T1'], 0, '#1D1D1F', 'PLA', 'Elegoo', 455, { spool_id: 17 }),
      lane('lane3', 'Turtle_1', 3, ['T2'], 1, '#D7263D', 'PETG', 'Sunlu', 92),
      lane('lane4', 'Turtle_1', 4, ['T3'], 1, '', '', '', 0),
      lane('lane5', 'Bambu_AMS_1', 1, ['T4'], 2, '#2E86DE', 'PLA', 'Bambu Lab', 730, { tool_loaded: true }),
      lane('lane6', 'Bambu_AMS_1', 2, ['T5'], 2, '#F4D03F', 'PLA', 'Bambu Lab', 610, { multi_colors: ['#F4D03F', '#E67E22', '#C0392B'] }),
      lane('lane7', 'Bambu_AMS_1', 3, ['T6'], 3, '#27AE60', 'PETG', 'Bambu Lab', 980),
      lane('lane8', 'Bambu_AMS_1', 4, ['T7'], 3, '#8E44AD', 'TPU', 'Bambu Lab', 35),
    ],
    max_tool: 7,
  };
  afc.lanes.forEach((l) => { l.unit_type = l.unit === 'Turtle_1' ? 'Box_Turtle' : 'BambuAMS'; });

  const project = {
    sliced: true,
    filaments: [
      { id: 0, color: '#F2F2F2', type: 'PLA', grams: 38.2, used: true, preset: 'Polymaker PolyLite PLA @U1' },
      { id: 1, color: '#2B7BD8', type: 'PLA', grams: 12.9, used: true, preset: 'Bambu PLA Basic @U1' },
      { id: 2, color: '#C8102E', type: 'PETG', grams: 104.5, used: true, preset: 'Generic PETG @U1' },
      { id: 3, color: '#111111', type: 'PLA', grams: 6.1, used: true, preset: 'Generic PLA @U1' },
      { id: 4, color: '#2ECC71', type: 'PETG', grams: 22.0, used: true, preset: 'Generic PETG @U1' },
      { id: 5, color: '#FFFFFF', type: 'PLA', grams: 0, used: false, preset: 'Generic PLA @U1' },
    ],
    suggestion: ['lane1', 'lane5', 'lane3', 'lane2', 'lane7', ''],
  };

  function state(includeProject) {
    const s = { connected: true, afc: JSON.parse(JSON.stringify(afc)) };
    if (includeProject) s.project = JSON.parse(JSON.stringify(project));
    return s;
  }

  function reply(header, code, message, data) {
    setTimeout(() => {
      window.postMessage(JSON.stringify({ header: header, payload: { code: code, message: message, data: data } }), '*');
    }, 120);
  }

  window.wx = {
    postMessage: function (raw) {
      const msg = JSON.parse(raw);
      const cmd = msg.payload.cmd;
      const p = msg.payload.params || {};
      const l = p.lane && afc.lanes.find((x) => x.name === p.lane);
      switch (cmd) {
        case 'sw_AFC_GetState':
          return reply(msg.header, 200, 'OK', state(p.project));
        case 'sw_AFC_EditLane':
          if (!l) return reply(msg.header, -1, 'unknown lane');
          Object.keys(p.changes).forEach((k) => {
            if (k === 'map') {
              const other = afc.lanes.find((x) => x.maps.includes(p.changes.map));
              if (other) other.maps = l.maps;
              l.maps = [p.changes.map];
            } else if (k === 'nozzle_temp' || k === 'bed_temp' || k === 'color' || k === 'material' || k === 'weight' || k === 'spool_id') {
              l[k] = p.changes[k];
            }
          });
          return reply(msg.header, 200, 'OK', state(false));
        case 'sw_AFC_ApplyMapping':
          p.mapping.forEach((m) => {
            const key = 'T' + m.tool;
            const target = afc.lanes.find((x) => x.name === m.lane);
            const holder = afc.lanes.find((x) => x.maps.includes(key));
            if (holder && target && holder !== target) { holder.maps = target.maps; target.maps = [key]; }
          });
          return reply(msg.header, 200, 'OK', state(true));
        case 'sw_AFC_LaneAction':
          return reply(msg.header, 200, 'OK', Object.assign(state(false), { error: 'Demo page: nothing moved' }));
        default:
          return reply(msg.header, 200, 'OK', {});
      }
    },
  };
})();
