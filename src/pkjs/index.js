var stations = require('./lib/stations');
var bundleLib = require('./lib/bundle');
var favsync = require('./lib/favsync');
var config = require('./lib/config');
var agencies = require('./lib/agencies');
var alertsLib = require('./lib/alerts');

// #ifdef SCREENSHOT_EMU
var SCREENSHOT_EMU = true;
var PREDEFINED_API_KEY = ""; // <-- Insert your Golemio API key here
var PREDEFINED_FAVS = [
  { id: "Muzeum", name: "Muzeum", label: "Metro A/C", agency: "pid", filterLines: "A, C" },
  { id: "Hlavní nádraží", name: "Hlavní nádraží", label: "Metro C", agency: "pid", filterLines: "C" },
  { id: "Anděl", name: "Anděl", label: "Metro B/Tram", agency: "pid", filterLines: "B, 9, 10" }
];
// #endif

function loadMirror() {
  var m;
  try { m = JSON.parse(localStorage.getItem('mta_favs')); } catch (e) {}
  if (!m) m = { nearestPos: 0, favs: [], apiKey: '', nearestFavsOnly: false };
  if (SCREENSHOT_EMU) {
    if (PREDEFINED_API_KEY) m.apiKey = PREDEFINED_API_KEY;
    if (!m.favs || m.favs.length === 0) {
      m.favs = PREDEFINED_FAVS.slice();
      m.nearestPos = 254;
    } else {
      for (var p = 0; p < PREDEFINED_FAVS.length; p++) {
        var pf = PREDEFINED_FAVS[p];
        for (var f = 0; f < m.favs.length; f++) {
          if (m.favs[f].id === pf.id && !m.favs[f].filterLines) {
            m.favs[f].filterLines = pf.filterLines;
            break;
          }
        }
      }
    }
  }
  return m;
}
function saveMirror(obj) {
  try { localStorage.setItem('mta_favs', JSON.stringify(obj)); } catch (e) {}
}
// Clock mode (0 Auto, 1 12h, 2 24h) mirrors the watch's setting so the config page
// opens on the current value. The watch is the source of truth: it pushes its mode
// on connect and on any watch-side change; the config page can push a new value back.
function loadClock() {
  var v = parseInt(localStorage.getItem('mta_clock'), 10);
  return (v === 1 || v === 2) ? v : 0;
}
function saveClock(v) {
  try { localStorage.setItem('mta_clock', String(v)); } catch (e) {}
}

// `token` (the watch's per-request id, echoed back) lets the watch drop stale
// responses from a station it has since navigated away from. Omitted when there
// is no originating request (e.g. the startup ack).
function sendError(code, token) {
  var msg = { ErrorCode: code };
  if (token != null) msg.Req = token;
  Pebble.sendAppMessage(msg);
}

function agencyForStation(station) {
  return agencies.get(station.agency || 'pid');
}

function refreshFor(station, token) {
  var agency = agencyForStation(station);
  agency.getArrivals(station, function (errCode, res) {
    if (errCode) { sendError(errCode, token); return; }
    var bytes = bundleLib.encodeBundle(res.station.id, res.station.name, res.model, res.epoch);
    var bundleMsg = { Bundle: Array.prototype.slice.call(bytes) };
    if (token != null) bundleMsg.Req = token;
    // Pebble has a single AppMessage outbox, and the C inbox handler treats an
    // Alerts message as mutually exclusive with a Bundle message (it returns
    // early when Alerts is present). So send the Bundle first, then send the
    // Alerts string only AFTER the bundle send is acked — never in one dict,
    // never back-to-back synchronously. Sending the (possibly empty) alerts
    // string clears any stale alert on the watch, matching prior behavior.
    Pebble.sendAppMessage(bundleMsg, function () {
      // The watch stores alerts in a 700-byte buffer and the AppMessage inbox
      // is right-sized (2 KB), so cap the string here — an over-long alert
      // would otherwise be dropped by the inbox instead of arriving truncated.
      // Clamped in UTF-8 BYTES (the watch buffer's unit), not JS characters.
      var alertsMsg = { Alerts: alertsLib.clampBytes((res.alerts || []).join('\n'), 690) };
      if (token != null) alertsMsg.Req = token;
      Pebble.sendAppMessage(alertsMsg);
    });
  });
}

function handleRequest(msg) {
  var token = msg.Req;
  if (msg.UseNearest) {
    navigator.geolocation.getCurrentPosition(
      function (p) {
        var mirror = loadMirror();
        var st = null;
        var preferFav = mirror.nearestFavsOnly || msg.UseNearest === 2;
        var selectedFav = null;
        if (preferFav && mirror.favs && mirror.favs.length > 0) {
          var minDist = Infinity;
          for (var i = 0; i < mirror.favs.length; i++) {
            var fs = stations.getStation(mirror.favs[i].id, mirror.favs[i].agency);
            if (fs && fs.lat) {
              var dx = fs.lon - p.coords.longitude, dy = fs.lat - p.coords.latitude;
              var dist = dx*dx + dy*dy;
              if (dist < minDist) {
                minDist = dist;
                st = fs;
                selectedFav = mirror.favs[i];
              }
            }
          }
        }
        if (!st) {
          st = stations.nearestStation(p.coords.latitude, p.coords.longitude);
        } else if (selectedFav) {
          st.filterLines = selectedFav.filterLines;
        }

        if (st) {
          st.apiKey = mirror.apiKey;
          refreshFor(st, token);
        } else sendError(2, token);
      },
      function (e) { console.log('[mta] geo FAIL ' + (e && e.message)); sendError(1, token); },
      { timeout: 15000, maximumAge: 5000 }
    );
  } else if (msg.StationId) {
    // Resolve by (agency, id) so colliding ids (MTA vs WMATA "A02") pick the right
    // system. msg.Agency is absent for legacy favorites → id-only fallback.
    var st = stations.getStation(msg.StationId, msg.Agency);
    if (st) {
      var mirror = loadMirror();
      for (var i = 0; i < mirror.favs.length; i++) {
        if (mirror.favs[i].id === msg.StationId) {
          st.filterLines = mirror.favs[i].filterLines;
          break;
        }
      }
      if (SCREENSHOT_EMU && !st.filterLines) {
        for (var p = 0; p < PREDEFINED_FAVS.length; p++) {
          if (PREDEFINED_FAVS[p].id === msg.StationId) {
            st.filterLines = PREDEFINED_FAVS[p].filterLines;
            break;
          }
        }
      }
      st.apiKey = mirror.apiKey;
      refreshFor(st, token);
    } else sendError(2, token);
  }
}

Pebble.addEventListener('ready', function () {
  Pebble.sendAppMessage({ ErrorCode: 0 });
  Pebble.sendAppMessage({ FavReq: 1 });
});
Pebble.addEventListener('appmessage', function (e) {
  if (e.payload && e.payload.FavSync) {
    var newMirror = favsync.decodeFavList(e.payload.FavSync);
    var oldMirror = loadMirror();
    for (var i=0; i<newMirror.favs.length; i++) {
       for (var j=0; j<oldMirror.favs.length; j++) {
         if (oldMirror.favs[j].id === newMirror.favs[i].id && oldMirror.favs[j].agency === newMirror.favs[i].agency) {
           newMirror.favs[i].filterLines = oldMirror.favs[j].filterLines;
           break;
         }
       }
    }
    newMirror.apiKey = oldMirror.apiKey;
    newMirror.nearestFavsOnly = (newMirror.nearestPos === 254);
    saveMirror(newMirror);
    return;
  }
  if (e.payload && e.payload.Clock != null) {   // watch pushed its clock mode
    saveClock(e.payload.Clock);
    return;
  }
  handleRequest(e.payload);
});

// Give legacy favorites (saved before agency tracking) an agency by looking up
// their id, so a re-save persists it. Best-effort id-only lookup — fine since the
// only colliding ids are MTA/WMATA, where the legacy default already resolved to MTA.
function backfillAgency(favs) {
  return (favs || []).map(function (f) {
    if (f.agency) return f;
    var st = stations.getStation(f.id);
    var c = {}; for (var k in f) if (f.hasOwnProperty(k)) c[k] = f[k];
    c.agency = st ? st.agency : '';
    return c;
  });
}

Pebble.addEventListener('showConfiguration', function () {
  var m = loadMirror();
  var db = stations._db.map(function (s) {
    // alt: member platform names of a merged complex ("6 Av" inside "14 St"),
    // searchable and shown on the result row; omitted when absent.
    // sys: lets the page label pure-PATH stations "· PATH", same as the watch.
    return { id: s.id, name: s.name, lines: s.lines, agency: s.agency, alt: s.alt, sys: s.sys };
  });
  var html = config.buildConfigHtml(m.nearestPos, backfillAgency(m.favs), db, loadClock(), m.apiKey, m.nearestFavsOnly);
  Pebble.openURL('data:text/html,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var payload;
  try { payload = JSON.parse(decodeURIComponent(e.response)); } catch (err) { return; }
  if (!payload) return;
  if (payload.favs) {
    payload.nearestFavsOnly = (payload.nearestPos === 254);
    saveMirror(payload);
  }
  // The outbox is single-slot: a second send before the first is acked returns BUSY
  // and is dropped. So send Clock first, then FavSet only after Clock is acked.
  function sendFavSet() {
    if (!payload.favs) return;
    var bytes = favsync.encodeFavList(payload.nearestPos, payload.favs);
    Pebble.sendAppMessage({ FavSet: Array.prototype.slice.call(bytes) });
  }
  if (payload.clock != null) {
    var cv = payload.clock | 0;
    saveClock(cv);
    Pebble.sendAppMessage({ Clock: cv }, sendFavSet, sendFavSet);
  } else {
    sendFavSet();
  }
});
