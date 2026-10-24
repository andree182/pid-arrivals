const { test } = require('node:test');
const assert = require('node:assert');
const agencies = require('./agencies');
const mta = agencies.get('mta');

test('mta directionWord maps (route,dir) to the borough word', () => {
  assert.strictEqual(mta._directionWord('Q', 'N'), 'MANHATTAN');
  assert.strictEqual(mta._directionWord('Q', 'S'), 'BROOKLYN');
  assert.strictEqual(mta._directionWord('M', 'N'), '');     // NONE -> empty
  assert.strictEqual(mta._directionWord('SIR', 'S'), '');
  assert.strictEqual(mta._directionWord('1', 'N'), 'BRONX');
});

test('registry exposes mta with a direct transport', () => {
  assert.strictEqual(mta.id, 'mta');
  assert.strictEqual(mta.transport, 'direct');
  assert.strictEqual(typeof mta.getArrivals, 'function');
});

test('get() defaults to pid for an unknown agency id', () => {
  assert.strictEqual(agencies.get('zzz').id, 'pid');
});

test('stripDiacritics strips accents from Czech characters', () => {
  assert.strictEqual(agencies._stripDiacritics('Nádraží Modřany'), 'nadrazi modrany');
  assert.strictEqual(agencies._stripDiacritics('Zličín'), 'zlicin');
  assert.strictEqual(agencies._stripDiacritics('Černý Most'), 'cerny most');
  assert.strictEqual(agencies._stripDiacritics('Štěpánská'), 'stepanska');
});

test('registry resolves the proxied CTA agency', () => {
  const c = agencies.get('cta');
  assert.strictEqual(c.id, 'cta');
  assert.strictEqual(c.transport, 'proxied');
  assert.strictEqual(typeof c.getArrivals, 'function');
});

test('stalled MTA feed and alert requests time out instead of hanging', () => new Promise((resolve) => {
  // Stalled-connection simulation: nothing ever fires unless the code set
  // xhr.timeout + ontimeout. Without them this used to spin forever.
  var requests = 0, timeouts = 0;
  global.XMLHttpRequest = function () {
    requests++;
    this.open = function () {};
    this.send = function () {
      var self = this;
      setTimeout(function () {
        if (typeof self.ontimeout === 'function' && self.timeout > 0) { timeouts++; self.ontimeout(); }
        else if (self.onerror) self.onerror(new Error('stalled'));
      }, 0);
    };
  };
  mta.getArrivals({ id: '127', name: 'Times Sq-42 St', lines: ['1'] }, function (err) {
    assert.strictEqual(err, 3);   // every feed dead -> offline
    assert.strictEqual(timeouts, requests, 'every XHR must set timeout + ontimeout');
    resolve();
  });
}));

test('registry resolves the proxied WMATA agency', () => {
  const w = agencies.get('wmata');
  assert.strictEqual(w.id, 'wmata');
  assert.strictEqual(w.transport, 'proxied');
  assert.strictEqual(typeof w.getArrivals, 'function');
});

// XHR mock routed by URL: each route is a function(xhr) that completes the
// request. Feed XHRs read xhr.response (arraybuffer); alerts reads responseText.
function mockXhr(routes) {
  global.XMLHttpRequest = function () {
    var self = this;
    this.open = function (m, url) { self._url = url; };
    this.send = function () {
      setTimeout(function () {
        for (var key in routes) {
          if (self._url.indexOf(key) >= 0) { routes[key](self); return; }
        }
        self.status = 404; self.onload();
      }, 0);
    };
  };
}
function ok(xhr, body) { xhr.status = 200; xhr.response = body; xhr.onload(); }

const { buildTrip } = require('./fixtures/make-fixture');
const FUTURE = Math.floor(Date.now() / 1000) + 300;

// 14 St (6th/7th Av) needs four feeds; a feed that answers 200 with an EMPTY
// body (CDN load-shed) must surface its lines as NO DATA — not silently drop
// them ("it's like the other lines don't even exist").
test('a feed answering 200 with an empty body surfaces its lines as NO DATA', () => new Promise((resolve) => {
  mockXhr({
    'subway-alerts': (x) => { x.status = 200; x.responseText = '{"entity":[]}'; x.onload(); },
    'gtfs-bdfm': (x) => ok(x, new ArrayBuffer(0)),                                  // empty body, F/M lines
    'gtfs-l': (x) => ok(x, buildTrip('L', [['L02N', FUTURE], ['L06N', FUTURE + 600]]).buffer),
    'gtfs': (x) => ok(x, buildTrip('1', [['132N', FUTURE], ['101N', FUTURE + 900]]).buffer),
  });
  mta.getArrivals({ id: '132', name: '14 St', ids: ['132', 'D19', 'L02'], lines: ['1', 'F', 'M', 'L'] }, function (err, res) {
    assert.strictEqual(err, null);
    var byLine = {};
    res.model.forEach(function (m) { byLine[m.line] = m; });
    assert.ok(byLine['1'] && byLine['1'].directions.length, 'live 1 train shows');
    assert.ok(byLine['L'] && byLine['L'].directions.length, 'live L train shows');
    assert.ok(byLine['F'], 'F must still exist as a NO DATA row');
    assert.ok(byLine['M'], 'M must still exist as a NO DATA row');
    assert.strictEqual(byLine['F'].directions.length, 0);
    assert.ok(/no live arrivals/i.test(byLine['F'].notice));
    resolve();
  });
}));

// Belt and braces for "the other lines don't even exist": every line the
// station serves must appear on the board even when its feed is HEALTHY but
// carries no trains for this station (planned work skipping the stop, an
// off-hours service pattern, a feed shape we didn't anticipate).
test('a served line absent from a healthy feed still appears as NO DATA', () => new Promise((resolve) => {
  mockXhr({
    'subway-alerts': (x) => { x.status = 200; x.responseText = '{"entity":[]}'; x.onload(); },
    // bdfm healthy, but carries only F trains — no M anywhere.
    'gtfs-bdfm': (x) => ok(x, buildTrip('F', [['D19N', FUTURE], ['F01N', FUTURE + 600]]).buffer),
    // l healthy, but its only trip serves OTHER stations (work skipping 6 Av).
    'gtfs-l': (x) => ok(x, buildTrip('L', [['L06N', FUTURE], ['L08N', FUTURE + 600]]).buffer),
    'gtfs': (x) => ok(x, buildTrip('1', [['132N', FUTURE], ['101N', FUTURE + 900]]).buffer),
  });
  mta.getArrivals({ id: '132', name: '14 St', ids: ['132', 'D19', 'L02'], lines: ['1', 'F', 'M', 'L'] }, function (err, res) {
    assert.strictEqual(err, null);
    var byLine = {};
    res.model.forEach(function (m) { byLine[m.line] = m; });
    ['1', 'F', 'M', 'L'].forEach(function (l) { assert.ok(byLine[l], l + ' present on the board'); });
    assert.ok(byLine['1'].directions.length, '1 is live');
    assert.ok(byLine['F'].directions.length, 'F is live');
    assert.strictEqual(byLine['M'].directions.length, 0, 'M is a notice row');
    assert.ok(/no live arrivals/i.test(byLine['M'].notice));
    assert.strictEqual(byLine['L'].directions.length, 0, 'L (skipping this stop) is a notice row');
    assert.ok(/no live arrivals/i.test(byLine['L'].notice));
    // Live lines come first so the watch's 8-line cap can only ever crowd out
    // placeholders, never a line that has real trains.
    assert.deepStrictEqual(res.model.map(function (m) { return m.line; }).slice(0, 2), ['1', 'F']);
    resolve();
  });
}));

test('an empty feed body is retried once and recovers', () => new Promise((resolve) => {
  var bdfmCalls = 0;
  mockXhr({
    'subway-alerts': (x) => { x.status = 200; x.responseText = '{"entity":[]}'; x.onload(); },
    'gtfs-bdfm': (x) => {
      bdfmCalls++;
      if (bdfmCalls === 1) return ok(x, new ArrayBuffer(0));                        // first try: empty
      ok(x, buildTrip('F', [['D19N', FUTURE], ['F01N', FUTURE + 600]]).buffer);     // retry: real data
    },
    'gtfs': (x) => ok(x, buildTrip('1', [['132N', FUTURE], ['101N', FUTURE + 900]]).buffer),
  });
  mta.getArrivals({ id: '132', name: '14 St', ids: ['132', 'D19'], lines: ['1', 'F'] }, function (err, res) {
    assert.strictEqual(err, null);
    assert.strictEqual(bdfmCalls, 2, 'empty body retried once');
    var f = res.model.filter(function (m) { return m.line === 'F'; })[0];
    assert.ok(f && f.directions.length, 'F recovered on retry');
    resolve();
  });
}));
