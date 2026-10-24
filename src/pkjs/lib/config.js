var lineSummary = require('./stations').lineSummary;

// JSON.stringify does not escape "</script>" or "<!--", so a free-text label
// could break out of the <script type="application/json"> island. Neutralize
// both sequences before embedding.
function safeJson(obj) {
  return JSON.stringify(obj).replace(/<\//g, '<\\/').replace(/<!--/g, '<\\u0021--');
}

// Station-search tokenizer: lowercase, punctuation (-/·&.) as word breaks,
// ordinal suffixes dropped (14th -> 14), and the long forms riders type folded
// onto the dataset's abbreviations (avenue -> Av, square -> Sq, ...). Defined as
// real functions (not page-string code) so they are unit-testable; their source
// is injected into the config page via Function.prototype.toString, so they
// must stay ES5 and self-contained.
function normTokens(s) {
  var MAP = { avenue: 'av', ave: 'av', street: 'st', square: 'sq', road: 'rd',
              boulevard: 'blvd', parkway: 'pkwy', place: 'pl', plaza: 'plz',
              heights: 'hts', center: 'ctr', centre: 'ctr', fort: 'ft',
              terrace: 'ter', junction: 'jct',
              first: '1', second: '2', third: '3', fourth: '4', fifth: '5',
              sixth: '6', seventh: '7', eighth: '8', ninth: '9', tenth: '10' };
  var str = String(s).toLowerCase();
  if (typeof str.normalize === 'function') {
    str = str.normalize('NFD').replace(/[\u0300-\u036f]/g, '');
  } else {
    var dmap = {
      'á':'a', 'č':'c', 'ď':'d', 'é':'e', 'ě':'e', 'í':'i', 'ň':'n', 'ó':'o',
      'ř':'r', 'š':'s', 'ť':'t', 'ú':'u', 'ů':'u', 'ý':'y', 'ž':'z'
    };
    str = str.replace(/[áčďéěíňóřšťúůýž]/g, function(c) { return dmap[c] || c; });
  }
  var words = str
    .replace(/(\d+)(st|nd|rd|th)\b/g, '$1')    // ordinals first: 14th -> 14 (before the
    .replace(/(\d)([a-z])/g, '$1 $2')          // glued split would leave a stray "th")
    .replace(/([a-z])(\d)/g, '$1 $2')          // glued: 6av -> 6 av, av6 -> av 6
    .replace(/[^a-z0-9]+/g, ' ').split(' ');
  var out = [];
  for (var i = 0; i < words.length; i++) {
    var w = words[i];
    if (w) out.push(MAP[w] || w);
  }
  return out;
}
// Every query token must prefix-match a station-name token (the display name or
// any member platform's alt name — "6 Av" inside the merged "14 St"), or name
// itself as a set of line bullets the station serves ("fml123" -> the
// F/M/L/1/2/3 complex; "sir" -> the SIR as a whole bullet). Empty queries match
// nothing.
function stationMatches(q, name, lines, alt) {
  var qt = normTokens(q), nt = normTokens(name);
  for (var a = 0; alt && a < alt.length; a++) nt = nt.concat(normTokens(alt[a]));
  if (!qt.length) return false;
  lines = lines || [];
  var lineSet = {};
  for (var i = 0; i < lines.length; i++) lineSet[String(lines[i]).toLowerCase()] = true;
  for (var t = 0; t < qt.length; t++) {
    var tok = qt[t], hit = false;
    for (var j = 0; j < nt.length && !hit; j++) hit = nt[j].indexOf(tok) === 0;
    if (!hit && lineSet[tok]) hit = true;            // whole token is one bullet (SIR, Bl)
    if (!hit && lines.length) {                      // token as a SET of 1-char bullets
      hit = true;
      for (var k = 0; k < tok.length && hit; k++) hit = !!lineSet[tok.charAt(k)];
    }
    if (!hit) return false;
  }
  return true;
}
// Result ordering among matches: 0 when every query token exactly equals a name
// (or alt-name) token ("14 st" === "14 St"), 1 when some token only
// prefix-matched ("14" in "145 St"). Lower sorts first.
function stationRank(q, name, alt) {
  var qt = normTokens(q), nt = normTokens(name);
  for (var a = 0; alt && a < alt.length; a++) nt = nt.concat(normTokens(alt[a]));
  for (var t = 0; t < qt.length; t++) {
    var exact = false;
    for (var j = 0; j < nt.length && !exact; j++) exact = nt[j] === qt[t];
    if (!exact) return 1;
  }
  return 0;
}

function buildConfigHtml(nearestPos, favs, stationDB, clock, apiKey, nearestFavsOnly) {
  var initState = safeJson({ nearestPos: nearestPos, favs: favs, clock: clock | 0, apiKey: apiKey || '', nearestFavsOnly: !!nearestFavsOnly });
  var db = safeJson(stationDB);
  return '<!DOCTYPE html>\n' +
'<html><head><meta charset="utf-8">' +
'<meta name="viewport" content="width=device-width, initial-scale=1">' +
'<title>Transit Favorites</title><style>' +
'body{font-family:-apple-system,Helvetica,Arial,sans-serif;margin:0;background:#111;color:#eee}' +
'header{background:#0a84ff;color:#fff;padding:14px 16px;font-size:18px;font-weight:600}' +
'section{padding:12px 16px}h2{font-size:13px;text-transform:uppercase;color:#9aa;margin:8px 0}' +
'.fav{display:flex;align-items:center;gap:8px;background:#1c1c1e;border-radius:10px;padding:8px;margin:6px 0}' +
'.fav .meta{flex:1;min-width:0}.fav .name{font-size:15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}' +
'.fav input{width:100%;box-sizing:border-box;background:#2c2c2e;border:0;border-radius:6px;color:#fff;padding:6px;margin-top:4px;font-size:14px}' +
'.fav button{background:#3a3a3c;border:0;color:#fff;border-radius:6px;width:32px;height:32px;font-size:16px}' +
'.fav button.rm{background:#5a1f1f}' +
'#search{width:100%;box-sizing:border-box;background:#2c2c2e;border:0;border-radius:8px;color:#fff;padding:10px;font-size:15px}' +
'#results{list-style:none;margin:8px 0 0;padding:0}#results li{padding:10px;background:#1c1c1e;border-radius:8px;margin:4px 0;display:flex;align-items:center}' +
'#save{position:sticky;bottom:0;width:100%;border:0;background:#0a84ff;color:#fff;padding:16px;font-size:17px;font-weight:600}' +
'.hint{color:#888;font-size:12px;padding:0 16px 8px}' +
'.cb{display:inline-block;min-width:30px;text-align:center;font-size:10px;font-weight:700;color:#fff;border-radius:5px;padding:2px 5px;margin-right:6px;vertical-align:middle}' +
'.chip{background:#2c2c2e;border:0;color:#bbb;border-radius:14px;padding:5px 10px;margin:0 4px 6px 0;font-size:12px}.chip.on{background:#0a84ff;color:#fff}' +
'#chips{padding:8px 16px 0}' +
'.seg{display:flex;gap:0;border-radius:8px;overflow:hidden;background:#2c2c2e}' +
'.seg button{flex:1;background:transparent;border:0;color:#bbb;padding:10px;font-size:14px}' +
'.seg button.on{background:#0a84ff;color:#fff;font-weight:600}' +
'</style></head><body>' +
'<header>Transit Favorites</header>' +
'<section><h2>Your stations</h2><div id="favs"></div>' +
'<div class="hint" id="cap"></div></section>' +
'<section><h2>Clock</h2><div class="seg" id="clock">' +
'<button data-clk="0">Auto</button><button data-clk="1">12-hour</button><button data-clk="2">24-hour</button>' +
'</div><div class="hint">Shows the current time on the arrivals board. Auto follows your watch.</div></section>' +
'<section><h2>Stations list</h2><div class="seg" id="nmode" style="flex-direction:column;gap:1px;background:#111">' +
'<button data-nm="0" style="background:#2c2c2e">Start with nearest</button>' +
'<button data-nm="1" style="background:#2c2c2e">Only favorites</button>' +
'<button data-nm="2" style="background:#2c2c2e">End with nearest</button>' +
'</div><div class="hint">Configure the Nearest station slot in the watch menu.</div></section>' +
'<div id="chips"></div>' +
'<section id="golemio-sec"><h2>Golemio API Key</h2><input id="apiKey" value="" style="width:100%;box-sizing:border-box;background:#2c2c2e;border:0;border-radius:6px;color:#fff;padding:10px;font-size:14px" placeholder="Optional API key for PID agency">' +
'<div class="hint">Data provided by Golemio API under CC-BY license. <a href="https://api.golemio.cz/api-keys/auth/sign-in" target="_blank" style="color:#0a84ff;text-decoration:none">Get API Key</a></div></section>' +
'<section><h2>Add a station</h2>' +
'<input id="search" placeholder="Search stations…" autocomplete="off">' +
'<ul id="results"></ul></section>' +
'<button id="save">Save</button>' +
'<script type="application/json" id="init-state">' + initState + '</script>' +
'<script type="application/json" id="station-db">' + db + '</script>' +
'<script>(function(){' +
'var MAX=10;' +
'var state=JSON.parse(document.getElementById("init-state").textContent);' +
'var DB=JSON.parse(document.getElementById("station-db").textContent);' +
lineSummary.toString() + ';' +
// Mirrors stations.displayName so the stored favorite name and the watch's
// station footer read identically.
'function disp(s){if(s.sys==="path")return s.name+" \\u00b7 PATH";var ls=lineSummary(s.lines);return s.name+(ls?" ("+ls+")":"");}' +
normTokens.toString() + ';' +
stationMatches.toString() + ';' +
stationRank.toString() + ';' +
// One distinct, city-iconic color per agency (used for the row badge AND the
// filter chip). Kept visually separable so every city reads uniquely.
'var AGENCY_META={mta:{city:"NYC",c:"#0039A6"},cta:{city:"CHI",c:"#2A8FD4"},wmata:{city:"DC",c:"#C8102E"},marta:{city:"ATL",c:"#F2A900"},bart:{city:"SF",c:"#ED7B26"},mbta:{city:"BOS",c:"#00843D"},septa:{city:"PHL",c:"#7C3AED"},gcrta:{city:"CLE",c:"#008C95"},miami:{city:"MIA",c:"#EC008C"},baltimore:{city:"BAL",c:"#8DC63F"},skyline:{city:"HNL",c:"#FF6F61"},patco:{city:"PAT",c:"#BC0035"},trenurbano:{city:"SJU",c:"#C03230"},lametro:{city:"LA",c:"#0072BC"},pid:{city:"Prague",c:"#D9222A"}};' +
'function meta(s){return AGENCY_META[s&&s.agency]||{city:"",c:"#666"};}' +
'function colorForCity(city){for(var k in AGENCY_META)if(AGENCY_META[k].city===city)return AGENCY_META[k].c;return "#0a84ff";}' +
// Dark vs light text by perceived luminance, so gold/sky chips stay legible.
'function textOn(hex){var h=hex.replace("#","");var r=parseInt(h.substr(0,2),16),g=parseInt(h.substr(2,2),16),b=parseInt(h.substr(4,2),16);return (0.299*r+0.587*g+0.114*b)>150?"#111":"#fff";}' +
'function favStation(id){for(var i=0;i<DB.length;i++)if(DB[i].id===id)return DB[i];return null;}' +
'function badgeEl(s){var m=meta(s);if(!m.city)return null;var b=document.createElement("span");b.className="cb";b.style.background=m.c;b.style.color=textOn(m.c);b.textContent=m.city;return b;}' +
'var selCity="All";' +
'function cities(){var seen={},out=["All"];for(var i=0;i<DB.length;i++){var c=meta(DB[i]).city;if(c&&!seen[c]){seen[c]=1;out.push(c);}}return out;}' +
'function renderChips(){var c=document.getElementById("chips");c.innerHTML="";var cs=cities();if(cs.length<=2){c.style.display="none";return;}else{c.style.display="";}cs.forEach(function(city){var on=(city===selCity);var b=document.createElement("button");b.className="chip"+(on?" on":"");b.setAttribute("data-city",city);b.textContent=city;if(city!=="All"){var col=colorForCity(city);b.style.background=col;b.style.color=textOn(col);b.style.opacity=on?"1":"0.45";b.style.border=on?"2px solid #fff":"2px solid transparent";}b.onclick=function(){selCity=city;renderChips();search(document.getElementById("search").value);renderFavs();updateGolemio();};c.appendChild(b);});}' +
'function updateGolemio(){var sec=document.getElementById("golemio-sec");if(sec)sec.style.display=(selCity==="Prague"||cities().length<=2)?"block":"none";}' +
'function applyCity(s){return selCity==="All"||meta(s).city===selCity;}' +
'function getReturn(){var m=location.search.match(/return_to=([^&]+)/);return m?decodeURIComponent(m[1]):"pebblejs://close#";}' +
'function renderFavs(){' +
'var c=document.getElementById("favs");c.innerHTML="";' +
'state.favs.forEach(function(f,i){' +
'var fs=favStation(f.id);' +
// City filter + badge key off the favorite's own agency (collisions like MTA/WMATA
// "A02" would mis-badge from an id-only DB lookup); fs supplies any other fields.
'if(!applyCity(f.agency?f:(fs||{})))return;' +
'var row=document.createElement("div");row.className="fav";' +
'var metaDiv=document.createElement("div");metaDiv.className="meta";' +
'var nm=document.createElement("div");nm.className="name";' +
'var badge=badgeEl(f.agency?f:fs);if(badge)nm.appendChild(badge);' +
'var nameSpan=document.createElement("span");nameSpan.textContent=f.name;nm.appendChild(nameSpan);' +
'var inp=document.createElement("input");inp.placeholder="Label (optional)";inp.value=f.label||"";' +
'inp.oninput=function(){state.favs[i].label=inp.value;};' +
'var inpFilt=document.createElement("input");inpFilt.placeholder="Filter (e.g. 17, Modřany)";inpFilt.value=f.filterLines||"";' +
'inpFilt.oninput=function(){state.favs[i].filterLines=inpFilt.value;};' +
'metaDiv.appendChild(nm);metaDiv.appendChild(inp);metaDiv.appendChild(inpFilt);' +
'var up=document.createElement("button");up.textContent="\\u2191";up.onclick=function(){move(i,-1);};' +
'var dn=document.createElement("button");dn.textContent="\\u2193";dn.onclick=function(){move(i,1);};' +
'var rm=document.createElement("button");rm.className="rm";rm.textContent="\\u2715";rm.onclick=function(){state.favs.splice(i,1);renderFavs();};' +
'row.appendChild(metaDiv);row.appendChild(up);row.appendChild(dn);row.appendChild(rm);' +
'c.appendChild(row);});' +
'document.getElementById("cap").textContent=state.favs.length+"/"+MAX+" favorites";' +
'}' +
'function move(i,d){var j=i+d;if(j<0||j>=state.favs.length)return;var t=state.favs[i];state.favs[i]=state.favs[j];state.favs[j]=t;renderFavs();}' +
'function isFav(st){for(var k=0;k<state.favs.length;k++)if(state.favs[k].id===st.id&&state.favs[k].agency===st.agency)return true;return false;}' +
'function hintRow(ul,text){var li=document.createElement("li");li.style.color="#888";li.style.background="transparent";li.textContent=text;ul.appendChild(li);}' +
'function search(q){' +
'var ul=document.getElementById("results");ul.innerHTML="";if(!q||!q.replace(/\\s/g,"")){return;}' +
'var hits=[];' +
'for(var k=0;k<DB.length;k++){var s=DB[k];' +
'if(!applyCity(s))continue;' +
'if(!stationMatches(q,s.name,s.lines,s.alt))continue;' +
'hits.push([stationRank(q,s.name,s.alt),hits.length,s]);}' +
'hits.sort(function(a,b){return a[0]-b[0]||a[1]-b[1];});' +   // exact-token matches first; DB order within
'var shown=0,more=hits.length>25?hits.length-25:0;' +
'for(var h=0;h<hits.length&&shown<25;h++,shown++){' +
'(function(st){var li=document.createElement("li");' +
'var badge=badgeEl(st);if(badge)li.appendChild(badge);' +
'var nameSpan=document.createElement("span");nameSpan.textContent=disp(st);li.appendChild(nameSpan);' +
// Member platform names ("· 6 Av") disambiguate same-named complexes
// (the three 14 Sts) right in the result row.
'if(st.alt&&st.alt.length){var altSpan=document.createElement("span");altSpan.style.color="#9aa";altSpan.style.marginLeft="6px";altSpan.style.fontSize="13px";altSpan.textContent="\\u00b7 "+st.alt.join(" / ");li.appendChild(altSpan);}' +
'if(isFav(st)){li.style.opacity="0.5";var tick=document.createElement("span");tick.style.marginLeft="auto";tick.style.color="#6e6";tick.textContent="\\u2713 Added";li.appendChild(tick);}' +
'else li.onclick=function(){add(st,li);};' +
'ul.appendChild(li);})(hits[h][2]);}' +
'if(more)hintRow(ul,more+" more \\u2014 keep typing to narrow");' +
'if(!shown)hintRow(ul,"No stations match \\u2014 try fewer words, or line letters like FML");' +
'}' +
// Adding keeps the query and result list on screen; the tapped row flips to
// "Added" in place, so there is visible feedback and nearby variants stay
// pickable. A full list says so instead of silently ignoring the tap.
'function add(st,li){' +
'if(isFav(st))return;' +
'if(state.favs.length>=MAX){li.style.color="#f88";li.lastChild.textContent=disp(st)+" \\u2014 favorites full (10)";return;}' +
'state.favs.push({id:st.id,name:disp(st),label:"",filterLines:"",agency:st.agency});' +
'renderFavs();search(document.getElementById("search").value);' +
'}' +
'document.getElementById("search").addEventListener("input",function(e){search(e.target.value);});' +
// Clock segmented control: highlight the active mode, update state on tap.
'var selClock=(state.clock|0);var selNm=(state.nearestPos===254)?1:(state.nearestPos>0?2:0);' +
'function renderClock(){var seg=document.getElementById("clock");var bs=seg.getElementsByTagName("button");for(var i=0;i<bs.length;i++){var on=(parseInt(bs[i].getAttribute("data-clk"),10)===selClock);bs[i].className=on?"on":"";bs[i].onclick=(function(v){return function(){selClock=v;renderClock();};})(parseInt(bs[i].getAttribute("data-clk"),10));}}' +
'function renderNm(){var seg=document.getElementById("nmode");var bs=seg.getElementsByTagName("button");for(var i=0;i<bs.length;i++){var on=(parseInt(bs[i].getAttribute("data-nm"),10)===selNm);bs[i].style.background=on?"#0a84ff":"#2c2c2e";bs[i].style.color=on?"#fff":"#bbb";bs[i].onclick=(function(v){return function(){selNm=v;renderNm();};})(parseInt(bs[i].getAttribute("data-nm"),10));}}' +
'document.getElementById("save").addEventListener("click",function(){' +
'var nPos = (selNm===1) ? 254 : ((selNm===2) ? (state.favs.length || 99) : 0);' +
'var data=encodeURIComponent(JSON.stringify({nearestPos:nPos,favs:state.favs,clock:selClock,apiKey:document.getElementById("apiKey").value}));' +
'var r=getReturn();location=r+(r.indexOf("#")>=0?"":"#")+data;' +
'});' +
'document.getElementById("apiKey").value = state.apiKey || "";' +
'renderChips();renderFavs();renderClock();renderNm();updateGolemio();' +
'})();</script></body></html>';
}

module.exports = { buildConfigHtml: buildConfigHtml, _normTokens: normTokens, _stationMatches: stationMatches, _stationRank: stationRank };
