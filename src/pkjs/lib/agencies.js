var proxied = require('./proxied');
var stations = require('./stations');
var linesLib = require('./lines');
var gtfsrt = require('./gtfsrt');
var arrivalsLib = require('./arrivals');
var alertsLib = require('./alerts');

var ALERTS_URL = 'https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/camsys%2Fsubway-alerts.json';

// Borough WORD each (route, N/S) direction heads toward. "" for lines whose ends
// share a borough (shuttles, SIR) — the headsign disambiguates. (Was the 0–4
// dirCode enum in bundle.js; v6 carries the word, so it lives here now.)
var MAN = 'MANHATTAN', BKN = 'BROOKLYN', QNS = 'QUEENS', BRX = 'BRONX', NONE = '';
var DIR_WORD = {
  '1': { N: BRX, S: MAN }, '2': { N: BRX, S: BKN }, '3': { N: MAN, S: BKN },
  '4': { N: BRX, S: BKN }, '5': { N: BRX, S: BKN }, '6': { N: BRX, S: MAN },
  '7': { N: QNS, S: MAN },
  'A': { N: MAN, S: QNS }, 'C': { N: MAN, S: BKN }, 'E': { N: QNS, S: MAN },
  'B': { N: BRX, S: BKN }, 'D': { N: BRX, S: BKN }, 'F': { N: QNS, S: BKN },
  'M': { N: NONE, S: NONE },
  'G': { N: QNS, S: BKN },
  'J': { N: QNS, S: MAN }, 'Z': { N: QNS, S: MAN },
  'N': { N: QNS, S: BKN }, 'Q': { N: MAN, S: BKN }, 'R': { N: QNS, S: BKN }, 'W': { N: QNS, S: MAN },
  'L': { N: MAN, S: BKN },
  'S': { N: NONE, S: NONE }, 'SIR': { N: NONE, S: NONE }
};
function directionWord(route, dir) {
  var d = DIR_WORD[route];
  if (!d) return '';
  return dir === 'N' ? d.N : d.S;
}
function parseIsoDate(s) {
  var m = s.match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.\d+)?(?:Z|([+-])(\d{2}):(\d{2}))?$/);
  if (!m) return new Date(s).getTime();
  var year = parseInt(m[1], 10);
  var month = parseInt(m[2], 10) - 1;
  var day = parseInt(m[3], 10);
  var hour = parseInt(m[4], 10);
  var min = parseInt(m[5], 10);
  var sec = parseInt(m[6], 10);
  var utc = Date.UTC(year, month, day, hour, min, sec);
  if (m[7]) {
    var sign = m[7] === '+' ? 1 : -1;
    var offHours = parseInt(m[8], 10);
    var offMins = parseInt(m[9], 10);
    utc -= sign * (offHours * 3600 + offMins * 60) * 1000;
  }
  return utc;
}

function stripDiacritics(s) {
  if (!s) return '';
  var str = String(s).toLowerCase();
  if (typeof str.normalize === 'function') {
    return str.normalize('NFD').replace(/[\u0300-\u036f]/g, '');
  }
  var dmap = {
    'á':'a', 'č':'c', 'ď':'d', 'é':'e', 'ě':'e', 'í':'i', 'ň':'n', 'ó':'o',
    'ř':'r', 'š':'s', 'ť':'t', 'ú':'u', 'ů':'u', 'ý':'y', 'ž':'z'
  };
  return str.replace(/[áčďéěíňóřšťúůýž]/g, function(c) { return dmap[c] || c; });
}

function nowSecs() { return Math.floor(Date.now() / 1000); }

function fetchFeed(url, cb) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.responseType = 'arraybuffer';
  xhr.onload = function () {
    if (xhr.status === 200 && xhr.response) cb(null, new Uint8Array(xhr.response));
    else cb(new Error('HTTP ' + xhr.status));
  };
  xhr.onerror = function () { cb(new Error('network')); };
  // Stalled connections fire neither onload nor onerror; bound the wait.
  xhr.timeout = 15000;
  xhr.ontimeout = function () { cb(new Error('timeout')); };
  xhr.send();
}

// Attach color + direction word to the raw arrivals model from arrivals.js.
function decorate(model) {
  return model.map(function (ln) {
    var color = linesLib.colorForLine(ln.line);
    if (!ln.directions || ln.directions.length === 0) {
      return { line: ln.line, color: color, directions: [], notice: ln.notice || '' };
    }
    var dirs = ln.directions.map(function (d) {
      return { label: directionWord(ln.line, d.dir), dest: d.dest, times: d.times, exp: d.exp };
    });
    return { line: ln.line, color: color, directions: dirs };
  });
}

function getArrivals(station, cb) {
  var feeds = linesLib.feedGroups(station.lines);
  var rows = [], pendingFeeds = feeds.length, failedFeeds = 0;
  var alertsDone = false, suspensions = [], alerts = [];

  function finish() {
    if (!alertsDone || pendingFeeds > 0) return;
    if (feeds.length && failedFeeds === feeds.length && !suspensions.length) return cb(3);
    var now = nowSecs();
    var ids = (station.ids || [station.id]).concat(station.pathIds || []);
    var rawModel = arrivalsLib.buildArrivals(rows, ids, now);
    suspensions.forEach(function (s) {
      var running = rawModel.some(function (m) { return m.line === s.code; });
      if (!running) rawModel.push({ line: s.code, directions: [], notice: s.reason });
    });
    // EVERY line the station serves appears on the board, whatever the cause of
    // its absence — a failed feed, a healthy feed with no trains for this stop
    // (planned work skipping it, an off-hours service pattern), or a feed shape
    // we didn't anticipate. Absence renders as NO DATA ("No live arrivals"
    // prefix, same as SEPTA El/BSL), never as a line that doesn't exist.
    // Appended AFTER the live lines, so the watch's 8-line cap can only crowd
    // out placeholders, never a line with real trains.
    station.lines.forEach(function (l) {
      if (!rawModel.some(function (m) { return m.line === l; })) {
        rawModel.push({ line: l, directions: [], notice: 'No live arrivals right now' });
      }
    });
    if (!rawModel.length) return cb(4);
    cb(null, {
      epoch: now,
      station: { id: station.id, name: stations.displayName(station), agency: 'mta' },
      model: decorate(rawModel),
      alerts: alerts,
      suspensions: suspensions.map(function (s) { return { line: s.code, reason: s.reason }; })
    });
  }

  var ax = new XMLHttpRequest();
  ax.open('GET', ALERTS_URL, true);
  ax.onload = function () {
    if (ax.status === 200 && ax.responseText) {
      try {
        var feed = JSON.parse(ax.responseText);
        alerts = alertsLib.extractAlerts(feed, station.lines, nowSecs());
        suspensions = alertsLib.extractSuspensions(feed, station.lines, nowSecs());
      } catch (e) { console.log('[mta] alerts EXC ' + e.message); }
    }
    alertsDone = true; finish();
  };
  ax.onerror = function () { console.log('[mta] alerts network FAIL'); alertsDone = true; finish(); };
  ax.timeout = 10000;
  ax.ontimeout = function () { console.log('[mta] alerts TIMEOUT'); alertsDone = true; finish(); };
  ax.send();

  // Fetch + parse a feed; an unparseable or EMPTY body (an HTTP 200 with zero
  // entities — e.g. CDN load-shed) counts as a failure just like a network
  // error, and the whole attempt retries once. The subway and PATH run 24/7, so
  // a TripUpdate feed with no rows at all is never real data; without this
  // check it slipped through and silently dropped that feed's whole line group.
  function fetchRowsRetry(url, done) {
    function attempt(cb) {
      fetchFeed(url, function (err, buf) {
        if (err) return cb(err);
        var got;
        try { got = gtfsrt.extractStopTimes(buf); }
        catch (e) { return cb(e); }
        if (!got.length) return cb(new Error('empty feed'));
        cb(null, got);
      });
    }
    attempt(function (err, got) {
      if (!err) return done(null, got);
      attempt(done);
    });
  }
  feeds.forEach(function (f) {
    fetchRowsRetry(f.url, function (err, got) {
      if (err) {
        console.log('[mta] feed FAIL ' + err.message + ' ' + f.url);
        failedFeeds++;
      } else {
        rows = rows.concat(got);
      }
      pendingFeeds--; finish();
    });
  });
}

var MTA = {
  id: 'mta', name: 'MTA', transport: 'direct',
  getArrivals: getArrivals,
  _directionWord: directionWord
};

var PID = {
  id: 'pid', name: 'PID', transport: 'direct',
  getArrivals: function(station, cb) {
    var url = 'https://api.golemio.cz/v2/pid/departureboards?limit=15&names=' + encodeURIComponent(station.name);
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.setRequestHeader('X-Access-Token', station.apiKey || '');
    
    xhr.onload = function() {
      if (xhr.status === 200 && xhr.responseText) {
        try {
          var data = JSON.parse(xhr.responseText);
          var departures = data.departures || [];
          var now = nowSecs();
          
          var linesMap = {};
          var filters = station.filterLines ? station.filterLines.split(',').map(function(s){return stripDiacritics(s.trim());}).filter(function(s){return s;}) : [];
          
          departures.forEach(function(dep) {
            var line = dep.route.short_name;
            var dest = dep.trip.headsign;
            var normDest = dest ? stripDiacritics(dest) : '';
            
            if (filters.length > 0) {
              var match = false;
              for (var i=0; i<filters.length; i++) {
                var f = filters[i];
                if (line.toLowerCase() === f || (normDest && normDest.indexOf(f) >= 0)) {
                  match = true; break;
                }
              }
              if (!match) return;
            }
            
            var tsStr = dep.departure_timestamp.predicted || dep.departure_timestamp.scheduled || dep.arrival_timestamp.predicted || dep.arrival_timestamp.scheduled;
            if (!tsStr) return;
            var ts = Math.floor(parseIsoDate(tsStr) / 1000);
            
            if (!linesMap[line]) {
              linesMap[line] = {
                line: line,
                color: linesLib.colorForLine(line, 'pid'),
                directionsMap: {}
              };
            }
            if (!linesMap[line].directionsMap[dest]) {
              linesMap[line].directionsMap[dest] = {
                dest: dest,
                label: '',
                times: [],
                exp: []
              };
            }
            linesMap[line].directionsMap[dest].times.push(ts);
            linesMap[line].directionsMap[dest].exp.push(false);
          });
          
          var model = [];
          for (var l in linesMap) {
            var dirs = [];
            for (var d in linesMap[l].directionsMap) {
              var dirObj = linesMap[l].directionsMap[d];
              dirObj.times.sort(function(a, b) { return a - b; });
              dirs.push(dirObj);
            }
            model.push({
              line: l,
              color: linesMap[l].color,
              directions: dirs
            });
          }
          
          if (model.length === 0) {
            model.push({ line: '-', directions: [], notice: 'No live arrivals right now' });
          }
          
          cb(null, {
            epoch: now,
            station: { id: station.id, name: station.name, agency: 'pid' },
            model: model,
            alerts: [],
            suspensions: []
          });
        } catch(e) {
          cb(3);
        }
      } else {
        cb(3);
      }
    };
    xhr.onerror = function() { cb(3); };
    xhr.timeout = 15000;
    xhr.ontimeout = function() { cb(3); };
    xhr.send();
  }
};

var REGISTRY = {
  mta: MTA,
  cta: proxied.makeProxiedAgency({ id: 'cta', name: 'CTA' }),
  wmata: proxied.makeProxiedAgency({ id: 'wmata', name: 'Metro' }),
  marta: proxied.makeProxiedAgency({ id: 'marta', name: 'MARTA' }),
  bart: proxied.makeProxiedAgency({ id: 'bart', name: 'BART' }),
  mbta: proxied.makeProxiedAgency({ id: 'mbta', name: 'MBTA' }),
  septa: proxied.makeProxiedAgency({ id: 'septa', name: 'SEPTA' }),
  gcrta: proxied.makeProxiedAgency({ id: 'gcrta', name: 'RTA' }),
  miami: proxied.makeProxiedAgency({ id: 'miami', name: 'Metrorail' }),
  baltimore: proxied.makeProxiedAgency({ id: 'baltimore', name: 'MdMTA' }),
  skyline: proxied.makeProxiedAgency({ id: 'skyline', name: 'Skyline' }),
  patco: proxied.makeProxiedAgency({ id: 'patco', name: 'PATCO' }),
  trenurbano: proxied.makeProxiedAgency({ id: 'trenurbano', name: 'Tren Urbano' }),
  lametro: proxied.makeProxiedAgency({ id: 'lametro', name: 'Metro' }),
  pid: PID
};
function get(id) { return REGISTRY[id] || PID; }   // default to PID for legacy favorites

module.exports = { get: get, _registry: REGISTRY, _stripDiacritics: stripDiacritics };
