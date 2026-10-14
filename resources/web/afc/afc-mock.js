/*
 * Stand-in for the slicer bridge when the page is opened in a plain browser
 * with ?mock=1 and no window.wx. Laid out like a big AFC toolchanger: six
 * toolheads, an ACE 2 Pro, an OpenAMS, a Box Turtle and, through BridgeBox, a
 * Bambu AMS 2 Pro and an AMS HT. Used to work on the page and to screenshot
 * it; inert inside the slicer.
 */
(function () {
  'use strict';
  if (window.wx || !/[?&]mock=1/.test(location.search)) return;

  function lane(name, unit, index, tool, head, color, material, vendor, weight, extra) {
    return Object.assign({
      name: name, unit: unit, unit_type: '', index: index, maps: ['T' + tool], extruder: head ? 'extruder' + head : 'extruder',
      extruder_index: head, color: color, multi_colors: [], material: material, sub_type: '', vendor: vendor,
      filament_name: vendor ? vendor + ' ' + material : '', spool_id: 0, weight: weight, initial_weight: 1000,
      density: 1.24, nozzle_temp: material === 'PETG' ? 240 : material === 'ABS' ? 260 : 215, bed_temp: material === 'PETG' ? 70 : material === 'ABS' ? 90 : 60,
      prep: !!material, load: !!material, tool_loaded: false, has_filament: !!material, status: material ? 'Loaded' : 'None', td: '', flow_k: null,
    }, extra || {});
  }

  const afc = {
    present: true, version: '1.2.7', spoolman: true, spoolman_url: 'http://spoolman.local:7912', multiple_mapping: false,
    current_lane: 'lane1', extruders: ['extruder', 'extruder1', 'extruder2', 'extruder3', 'extruder4', 'extruder5'],
    units: [
      { name: 'Ace2_1', type: 'ACE2', lanes: ['lane0', 'lane1', 'lane2', 'lane3'],
        dryer: { kind: 'ace', model: 'ACE2', online: true, drying: false, rotate: false, max_temp: 70, target: 0, temperature: 24, humidity: 38, remaining_s: 0, duration_s: 0, state: 'stop', note: '' } },
      { name: 'AMS_1', type: 'OpenAMS', lanes: ['lane4', 'lane5', 'lane6', 'lane7'], dryer: null },
      { name: 'Turtle_1', type: 'Box_Turtle', lanes: ['lane8', 'lane9', 'lane10', 'lane11'], dryer: null },
      { name: 'Bambu_AMS_1', type: 'BambuAMS', lanes: ['lane12', 'lane13', 'lane14', 'lane15'],
        dryer: { kind: 'bambu', model: 'ams2', online: true, drying: true, rotate: false, max_temp: 65, target: 65, temperature: 61, humidity: 12, remaining_s: 11520, duration_s: 28800, state: '2', note: '' } },
      { name: 'Bambu_AMS_HT_1', type: 'BambuAMS', lanes: ['lane20'],
        dryer: { kind: 'bambu', model: 'ht', online: true, drying: false, rotate: false, max_temp: 85, target: 0, temperature: 26, humidity: 29, remaining_s: 0, duration_s: 0, state: '0', note: '' } },
      { name: 'Bambu_AMS_2', type: 'BambuAMS', lanes: [], dryer: null },
      { name: 'Tools', type: 'Toolchanger', lanes: [], dryer: null },
    ],
    lanes: [
      lane('lane0', 'Ace2_1', 1, 0, 0, '#F2F2F2', 'PLA', 'Polymaker', 640, { spool_id: 41, flow_k: 0.032 }),
      lane('lane1', 'Ace2_1', 2, 1, 1, '#1D1D1F', 'PLA', 'Elegoo', 996, { tool_loaded: true, status: 'Tooled' }),
      lane('lane2', 'Ace2_1', 3, 2, 2, '#E94B3C', 'PLA', 'Generic', 978, { tool_loaded: true, status: 'Tooled', spool_id: 133 }),
      lane('lane3', 'Ace2_1', 4, 3, 3, '', '', '', 0),
      lane('lane4', 'AMS_1', 1, 4, 4, '#2ECC71', 'PETG', 'Sunlu', 410, { tool_loaded: true, status: 'Tooled' }),
      lane('lane5', 'AMS_1', 2, 5, 4, '#F4D03F', 'PETG', 'Sunlu', 88),
      lane('lane6', 'AMS_1', 3, 6, 4, '', '', '', 0),
      lane('lane7', 'AMS_1', 4, 7, 4, '', '', '', 0),
      lane('lane8', 'Turtle_1', 1, 8, 5, '', '', '', 0),
      lane('lane9', 'Turtle_1', 2, 9, 5, '#D69090', 'PLA', 'Generic', 768, { tool_loaded: true, status: 'Tooled', spool_id: 40, flow_k: 0.028 }),
      lane('lane10', 'Turtle_1', 3, 10, 5, '#8E44AD', 'PLA', 'Overture', 530, { multi_colors: ['#8E44AD', '#2E86DE', '#16A085'] }),
      lane('lane11', 'Turtle_1', 4, 11, 5, '', '', '', 0),
      lane('lane12', 'Bambu_AMS_1', 1, 12, 0, '#0A2989', 'PLA', 'Bambu Lab', 60, { spool_id: 165 }),
      lane('lane13', 'Bambu_AMS_1', 2, 13, 0, '#FF6A13', 'ABS', 'Bambu Lab', 964, { spool_id: 163, flow_k: 0.0215 }),
      lane('lane14', 'Bambu_AMS_1', 3, 14, 0, '#FFFFFF', 'PLA', 'Bambu Lab', 1000),
      lane('lane15', 'Bambu_AMS_1', 4, 15, 0, '#0086D6', 'PLA', 'Bambu Lab', 483, { spool_id: 136 }),
      lane('lane20', 'Bambu_AMS_HT_1', 1, 20, 0, '#5B5B5B', 'PA', 'Bambu Lab', 750, { sub_type: 'CF' }),
    ],
    max_tool: 20,
  };
  const types = {};
  afc.units.forEach((u) => { types[u.name] = u.type; });
  afc.lanes.forEach((l) => { l.unit_type = types[l.unit]; });

  const project = {
    sliced: true,
    filaments: [
      { id: 0, color: '#F2F2F2', type: 'PLA', grams: 38.2, used: true, preset: 'Polymaker PolyLite PLA', spool_pa: 0.03 },
      { id: 1, color: '#2B4BA8', type: 'PLA', grams: 72.9, used: true, preset: 'Bambu PLA Basic', spool_pa: -1 },
      { id: 2, color: '#C8102E', type: 'PLA', grams: 104.5, used: true, preset: 'Generic PLA', spool_pa: -1 },
      { id: 3, color: '#FF7A1A', type: 'ABS', grams: 16.1, used: true, preset: 'Bambu ABS', spool_pa: -1 },
      { id: 4, color: '#2ECC71', type: 'PETG', grams: 22.0, used: true, preset: 'Generic PETG', spool_pa: -1 },
      { id: 5, color: '#FFFFFF', type: 'PLA', grams: 0, used: false, preset: 'Generic PLA', spool_pa: -1 },
    ],
    suggestion: ['lane0', 'lane12', 'lane2', 'lane13', 'lane4', ''],
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
              if (k === 'spool_id') l.flow_k = null;
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
        case 'sw_AFC_Dry': {
          const u = afc.units.find((x) => x.name === p.unit);
          if (!u || !u.dryer) return reply(msg.header, -1, p.unit + ' has no dryer');
          if (p.action === 'stop') Object.assign(u.dryer, { drying: false, target: 0, remaining_s: 0, duration_s: 0 });
          else Object.assign(u.dryer, { drying: true, target: p.temp, remaining_s: p.minutes * 60, duration_s: p.minutes * 60, rotate: !!p.rotate });
          return reply(msg.header, 200, 'OK', state(false));
        }
        default:
          return reply(msg.header, 200, 'OK', {});
      }
    },
  };
})();
