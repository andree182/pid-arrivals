# PID Arrivals

A Pebble watchapp showing live transit countdowns in Prague (PID — Pražská integrovaná doprava).

The code is based on ["Transit Arrivals"](https://github.com/davidtorcivia/transit-arrivals).

## Features

- Nearest station by GPS. Multi-entrance complexes match from the closest platform, not a single centroid.
- Up to 10 saved favorites with custom labels and line/headsign filters (e.g. `17, Modřany`).
- Station search and destination filters disregard diacritics (e.g. `zlicin` matches `Zličín`).
- Optional Golemio API key support configured in the mobile settings page.
- Two-item connection view per screen showing destination headsign, countdown to the soonest departure, and next upcoming minutes.
- The last board is cached on-watch and shown immediately at launch while fresh data loads.
- App glance: the launcher tile shows the next departure for the last station, expiring when that train leaves.
- Builds for basalt, chalk, diorite, emery, flint, and gabbro, with layouts tuned for rect and round.

<p align="center">
  <img src="screenshots/live-mbta-emery.png" width="170" alt="MBTA Red line" />
  <img src="screenshots/live-septa-emery.png" width="170" alt="SEPTA trolley" />
  <img src="screenshots/sched-trenurbano-emery.png" width="170" alt="Tren Urbano timetable times" />
  <img src="screenshots/septa-nodata-emery.png" width="170" alt="SEPTA B line NO DATA state" />
</p>
<p align="center">
  <img src="screenshots/live-gcrta-emery.png" width="170" alt="Cleveland RTA" />
  <img src="screenshots/live-marta-emery.png" width="170" alt="MARTA Red line" />
  <img src="screenshots/sched-patco-gabbro.png" width="196" alt="PATCO on the round display" />
  <img src="screenshots/no-coverage-emery.png" width="170" alt="Outside-coverage state" />
</p>

## Architecture

```
+-------------+  AppMessage   +------------------+     HTTPS      +---------------------+
|  Watch (C)  | <------------ |  Phone (PKJS)    | <------------> |     Golemio API     |
|  src/c/     |  binary v9    |  src/pkjs/       |  (PID direct)  |  api.golemio.cz     |
|  UI, cache, |  "bundle"     |  PID stations,   |                +---------------------+
|  favorites  | ------------> |  geolocation,    |
+-------------+   requests    |  Golemio key     |
                              +------------------+
```

- `src/c/`: the watchapp. Two-item connection list rendering, binary bundle decoder, chunked persist cache, favorites ring, boundary bounce animations, app glance.
- `src/pkjs/`: phone-side JS. Resolves the station (GPS or favorite), queries PID arrival data via Golemio API, applies line/destination filters with diacritic stripping, and encodes the wire bundle for the watch. (Underlying US systems from upstream Transit Arrivals remain in the codebase and can be toggled via `ONLY_PID` in `src/pkjs/lib/stations.js`).
- `cta-proxy/`: Cloudflare Worker proxy from upstream used for multi-agency routing when US systems are enabled.
- `tools/`: station database generation utilities (including `tools/update_prague_stations.py`).

## Building

Standard [Pebble SDK](https://developer.rebble.io/) workflow from the repo root:

```sh
pebble build
pebble install --emulator emery     # or basalt/chalk/diorite/flint/gabbro, or a watch
```

## Tests

Run the phone JS unit tests with Node:

```sh
node --test src/pkjs/lib/*.test.js
```

## License

MIT
