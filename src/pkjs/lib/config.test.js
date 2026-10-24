const test = require('node:test');
const assert = require('node:assert');
const { buildConfigHtml } = require('./config');

const DB = [
  { id: 'L16', name: 'DeKalb Av', lines: ['L'] },
  { id: 'R30', name: 'DeKalb Av', lines: ['B', 'Q', 'R'] },
  { id: '635', name: '14 St-Union Sq', lines: ['4', '5', '6'] },
];

function extractJson(html, varName) {
  const re = new RegExp('id="' + varName + '"[^>]*>([\\s\\S]*?)<\\/script>');
  const m = html.match(re);
  assert.ok(m, 'embedded ' + varName + ' script found');
  return JSON.parse(m[1]);
}

test('embeds the full station DB', () => {
  const html = buildConfigHtml(0, [], DB);
  const db = extractJson(html, 'station-db');
  assert.strictEqual(db.length, 3);
  assert.strictEqual(db[0].id, 'L16');
});

test('embeds current favorites with labels and nearestPos', () => {
  const favs = [{ id: 'L16', name: 'DeKalb Av (L)', label: 'Home' }];
  const html = buildConfigHtml(2, favs, DB);
  const state = extractJson(html, 'init-state');
  assert.strictEqual(state.nearestPos, 2);
  assert.deepStrictEqual(state.favs, favs);
});

test('escapes a label that tries to break out of the script island', () => {
  const evil = '</script><img src=x onerror=alert(1)>';
  const html = buildConfigHtml(0, [{ id: 'A', name: 'N', label: evil }], DB);
  // The raw "</script>" must not appear inside the JSON island (it is escaped),
  // so the extractor's first-</script> match still captures the whole island...
  const state = extractJson(html, 'init-state');
  assert.strictEqual(state.favs[0].label, evil);   // ...and the label survives intact
  assert.ok(html.indexOf('<\\/script><img') >= 0, 'breakout sequence is escaped');
});

test('uses neutral multi-agency branding (not MTA-specific)', () => {
  const html = buildConfigHtml(0, [], DB);
  assert.match(html, /<header>Transit Favorites<\/header>/);
  assert.ok(html.indexOf('>MTA Favorites<') < 0, 'no MTA-specific header text');
});

test('embeds an agency->city/color map and a badge renderer', () => {
  const db = [
    { id: 'R16', name: 'Times Sq', lines: ['Q'], agency: 'mta' },
    { id: '40380', name: 'Clark/Lake', lines: ['Bl'], agency: 'cta' },
  ];
  const html = buildConfigHtml(0, [], db);
  assert.match(html, /AGENCY_META=/);
  assert.match(html, /CHI/); assert.match(html, /NYC/);
  assert.match(html, /function badgeEl\(/);
});

test('renders city filter chips derived from the DB', () => {
  const db = [
    { id: 'R16', name: 'A', lines: ['Q'], agency: 'mta' },
    { id: '40380', name: 'B', lines: ['Bl'], agency: 'cta' },
  ];
  const html = buildConfigHtml(0, [], db);
  assert.match(html, /id="chips"/);
  assert.match(html, /data-city/);
  assert.match(html, /function applyCity\(/);
  assert.match(html, /\.cb\{/);   // badge CSS present
});

test('embeds the clock mode and renders the Auto/12h/24h control', () => {
  const html = buildConfigHtml(0, [], DB, 2);
  const state = extractJson(html, 'init-state');
  assert.strictEqual(state.clock, 2);                 // current mode round-trips
  assert.match(html, /id="clock"/);
  assert.match(html, /data-clk="0"[^>]*>Auto/);
  assert.match(html, /data-clk="1"[^>]*>12-hour/);
  assert.match(html, /data-clk="2"[^>]*>24-hour/);
  assert.match(html, /clock:selClock/);               // save payload carries the choice
});

test('clock defaults to 0 (Auto) when omitted', () => {
  const html = buildConfigHtml(0, [], DB);
  assert.strictEqual(extractJson(html, 'init-state').clock, 0);
});

test('produces a complete HTML document', () => {
  const html = buildConfigHtml(255, [], DB);
  assert.match(html, /^<!DOCTYPE html>/);
  assert.match(html, /<\/html>\s*$/);
});

// --- forgiving station search -----------------------------------------------
const { _stationMatches: matches } = require('./config');

test('search forgives ordinals and long street words ("14th street" finds "14 St")', () => {
  assert.ok(matches('14th st', '14 St', ['1']));
  assert.ok(matches('14th street', '14 St', ['1']));
  assert.ok(matches('union square', '14 St-Union Sq', ['4', '5', '6']));
  assert.ok(matches('dekalb avenue', 'DeKalb Av', ['L']));
  assert.ok(!matches('15th st', '14 St', ['1']), 'wrong number still misses');
});

test('search treats hyphens and slashes as word breaks', () => {
  assert.ok(matches('union sq', '14 St-Union Sq', []));
  assert.ok(matches('jackson library', 'Jackson/Library', []));
});

test('search matches a token of line bullets ("14 fml" finds the 6 Av complex)', () => {
  const SIXTH = ['1', '2', '3', 'F', 'M', 'L'];
  assert.ok(matches('14 fml', '14 St', SIXTH));
  assert.ok(matches('fml123', '14 St', SIXTH), 'pure bullet-set query');
  assert.ok(!matches('14 fml', '14 St', ['A', 'C', 'E', 'L']), 'bullets not served -> miss');
  assert.ok(matches('sir', 'St George', ['SIR']), 'multi-char bullet matched whole');
});

test('search matches member platform alt names ("6th ave" finds the 14 St complex)', () => {
  const SIXTH = ['1', '2', '3', 'F', 'M', 'L'];
  assert.ok(matches('6th ave', '14 St', SIXTH, ['6 Av']));
  assert.ok(matches('6 av', '14 St', SIXTH, ['6 Av']));
  assert.ok(matches('port authority', 'Times Sq-42 St', [], ['42 St-Port Authority Bus Terminal']));
  assert.ok(matches('bleecker', 'Broadway-Lafayette St', [], ['Bleecker St']));
  assert.ok(!matches('6 av', '14 St', ['A', 'C', 'E'], ['8 Av']), 'wrong avenue still misses');
});

test('search splits glued tokens and reads number words ("6av", "sixth ave")', () => {
  const SIXTH = ['1', '2', '3', 'F', 'M', 'L'];
  assert.ok(matches('6av', '14 St', SIXTH, ['6 Av']));
  assert.ok(matches('6ave', '14 St', SIXTH, ['6 Av']));
  assert.ok(matches('sixth ave', '14 St', SIXTH, ['6 Av']));
  assert.ok(matches('14st', '14 St', SIXTH, []));
  assert.ok(matches('fml123', '14 St', SIXTH, []), 'bullet-set query survives the split');
});

test('search is plain-substring tolerant and rejects empty queries', () => {
  assert.ok(matches('dekalb', 'DeKalb Av', []));
  assert.ok(!matches('', 'DeKalb Av', []));
  assert.ok(!matches('   ', 'DeKalb Av', []));
});

test('search disregards diacritics ("zlicin" matches "Zličín", and "Zličín" matches "Zlicin")', () => {
  assert.ok(matches('zlicin', 'Zličín', []));
  assert.ok(matches('Zličín', 'Zličín', []));
  assert.ok(matches('Zlicin', 'Zličín', []));
  assert.ok(matches('zli', 'Zličín', []));
  assert.ok(matches('modrany', 'Nádraží Modřany', []));
  assert.ok(matches('nadrazi modrany', 'Nádraží Modřany', []));
});

test('exact token matches rank ahead of prefix-only matches ("14 st": 14 St before 145 St)', () => {
  const { _stationRank: rank } = require('./config');
  assert.ok(rank('14 st', '14 St') < rank('14 st', '145 St'));
  assert.ok(rank('14th street', '14 St') < rank('14th street', '145 St'));
  assert.strictEqual(rank('dekalb', 'DeKalb Av'), 0);
});

test('config page embeds the shared matcher and gives feedback instead of clearing the search', () => {
  const html = buildConfigHtml(0, [], DB);
  assert.match(html, /function stationMatches\(/);     // same matcher injected into the page
  assert.match(html, /function normTokens\(/);
  assert.ok(html.indexOf('value="";search("")') < 0, 'add() must not blank the search box');
  assert.match(html, /Added/);                          // already-added rows say so
});

// Drive the real page script with a minimal DOM stub: type a query, tap a
// result, and check the add flow's feedback (favorite stored, query kept,
// tapped row flips to "Added" in place).
test('page behavior: searching "fml123" finds the 6 Av complex; tapping adds it in place', () => {
  function el(tag) {
    return {
      tag, children: [], style: {}, listeners: {}, attrs: {}, textContent: '', value: '',
      appendChild(c) { this.children.push(c); },
      set innerHTML(v) { if (v === '') this.children = []; },
      get innerHTML() { return ''; },
      addEventListener(ev, fn) { this.listeners[ev] = fn; },
      setAttribute(k, v) { this.attrs[k] = String(v); },
      getAttribute(k) { return this.attrs[k] != null ? this.attrs[k] : null; },
      getElementsByTagName(t) { return this.children.filter((c) => c.tag === t); },
    };
  }
  const db = [
    { id: '132', name: '14 St', lines: ['1', '2', '3', 'F', 'M', 'L', 'JS', 'H3'], agency: 'mta', alt: ['6 Av'] },
    { id: 'A31', name: '14 St', lines: ['A', 'C', 'E', 'L'], agency: 'mta', alt: ['8 Av'] },
  ];
  const html = buildConfigHtml(0, [], db, 0);
  const ids = {};
  ['favs', 'cap', 'chips', 'search', 'results', 'save', 'clock', 'apiKey', 'nmode'].forEach((i) => { ids[i] = el('div'); });
  for (let i = 0; i < 3; i++) {
    const b = el('button'); b.setAttribute('data-clk', i); ids.clock.appendChild(b);
  }
  const islands = html.match(/<script type="application\/json" id="(.*?)">([\s\S]*?)<\/script>/g);
  islands.forEach((s) => {
    const m = s.match(/id="(.*?)">([\s\S]*?)<\/script>/);
    ids[m[1]] = { textContent: m[2] };
  });
  const doc = { getElementById: (i) => ids[i], createElement: el };
  const script = html.match(/<script>([\s\S]*?)<\/script><\/body>/)[1];
  new Function('document', script)(doc);

  ids.search.value = 'fml123';
  ids.search.listeners.input({ target: { value: 'fml123' } });
  const rows = ids.results.children;
  assert.strictEqual(rows.length, 1, 'bullet query matches only the 6 Av complex');
  assert.ok(typeof rows[0].onclick === 'function');
  assert.ok(rows[0].children.some((c) => c.textContent === '14 St (123FML · PATH)'),
    'row label folds PATH bullets into one tag');

  rows[0].onclick();
  assert.match(ids.cap.textContent, /1\/10/, 'favorite stored');
  assert.strictEqual(ids.search.value, 'fml123', 'query survives the add');
  const after = ids.results.children[0];
  assert.strictEqual(after.onclick, undefined, 'added row no longer clickable');
  assert.ok(after.children.some((c) => /Added/.test(c.textContent)), 'row shows Added');

  // Alt names: "6th ave" finds the complex by its member platform name, and
  // the row carries the "· 6 Av" hint that tells the three 14 Sts apart.
  ids.search.listeners.input({ target: { value: '6th ave' } });
  const altRows = ids.results.children;
  assert.strictEqual(altRows.length, 1, 'alt-name query matches only the 6 Av complex');
  assert.ok(altRows[0].children.some((c) => /6 Av/.test(c.textContent)), 'row shows the platform name');
});
