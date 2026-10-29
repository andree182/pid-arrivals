#include <pebble.h>
#include <string.h>
#include "bundle.h"
#include "hero.h"
#include "states.h"
#include "favorites.h"
#include "favsync.h"
#include "transition.h"
#include "loading.h"
#include "arrival.h"

#define PERSIST_BUNDLE 1        // LEGACY single-key bundle cache; deleted on load (see bundle chunks)
#define PERSIST_SEL    5
#define PERSIST_NEAREST_POS 6
#define PERSIST_SEEN_HELP   7
#define PERSIST_VIEW        8   // packed (s_line << 8 | s_dir): last line+direction shown
#define PERSIST_VIEWMEM     9   // LEGACY pre-v8 ViewMem table (12-byte ids); deleted on load
// (keys 10..19 belong to favorites.c: PERSIST_FAV_BASE + i)
#define PERSIST_VIEWMEM2    20  // per-station last line+direction table (see ViewMem)
// Cached-bundle storage. persist_write_data silently caps a key at
// PERSIST_DATA_MAX_LENGTH (256 B) and a real multi-line bundle is up to ~828 B,
// so the cache is split across fixed 256-byte chunk keys with the total length
// under its own key. (The old single-key cache at PERSIST_BUNDLE never actually
// persisted anything beyond two lines.)
#define PERSIST_BUNDLE_LEN    25
#define PERSIST_BUNDLE_CHUNK0 26   // ..29: four chunks of up to 256 B = 1024 B
#define PERSIST_CLOCK         30   // ClockMode: 0 auto, 1 12h, 2 24h
#define BUNDLE_CHUNK_SZ  256
#define BUNDLE_CHUNKS    4
#define BUNDLE_CACHE_MAX (BUNDLE_CHUNK_SZ * BUNDLE_CHUNKS)

#define STALE_SECS 120   // data older than this is no longer trustworthy as "live"
#define CONNECT_TIMEOUT_MS 20000  // no bundle/error in this window -> "Can't reach phone"

// Upward warning triangle for the hero alert badge (16x14).
static const GPathInfo WARN_TRI = { 3, (GPoint[]){ {8, 0}, {16, 14}, {0, 14} } };

static Window *s_window;
static Layer *s_canvas;
static AppTimer *s_poll;
static AppTimer *s_launch_settle = NULL;  // launch flip-in fallback (see launch_settle_cb)
static Bundle s_bundle;
static bool s_have_bundle = false;
static int s_error = -1;        // -1 none; 0 ready; 1 no-loc; 2 bad-station; 3 offline; 4 no-trains; 5 no-phone (watchdog)
static uint8_t s_line = 0, s_dir = 0;
// Per-line direction memory: each line remembers the direction you last viewed it
// in, so scrolling away and back restores that line's own choice (line A stays
// Brooklyn, line B stays Manhattan) instead of carrying one direction across all
// lines. Indexed by line index in the current bundle; reset when the station
// changes. Kept in sync with s_dir whenever the user flips the current line.
static uint8_t s_line_dir[MAX_LINES] = {0};
static uint8_t s_sel = 0;       // ring index: 0..ring_len()-1
static bool s_startup = true;
int s_scroll_idx = 0;
static uint8_t s_nearest_pos = 0; // 0..favorites_count() = Nearest slot position
static uint8_t s_clock_mode = CLOCK_AUTO;   // footer clock: Auto/12h/24h (PERSIST_CLOCK)
static bool s_switching = false;// true between a ring switch and the next bundle
static char s_hint[FAV_NAME_LEN];
static char s_alerts[700];       // active service-alert headlines, "\n"-joined ("" = none)
static GPath *s_warn_path;       // warning-triangle badge on the hero screen
static Window *s_settings;
static MenuLayer *s_menu;

// Manage-favorites screen state.
static Window    *s_manage;
static MenuLayer *s_manage_menu;
static int        s_move_row = -1;   // -1 = scrolling; else the row being moved
static Window    *s_confirm;         // remove-confirmation window
static uint8_t    s_confirm_row = 0;
static TextLayer *s_confirm_msg, *s_confirm_hint;

// Marquee for the selected manage row's subtitle (the "instructions"): pause,
// scroll left to reveal the tail, pause, reset, loop. Only runs while a row's
// subtitle is wider than the cell.
#define MARQ_INSET_RECT 4
#define MARQ_INSET_ROUND 22
#define MARQ_DELAY_MS  1000          // pause before scrolling starts
#define MARQ_STEP_MS     60          // time between scroll steps
#define MARQ_STEP_PX      3          // pixels per scroll step
#define MARQ_END_MS    1400          // pause at the end before resetting
static int       s_marq_row = -1;    // selected manage row, or -1
static int       s_marq_x = 0;       // current horizontal scroll offset (px)
static int       s_marq_max = 0;     // how far the subtitle overflows the cell
static int       s_manage_w = 144;   // manage menu width, for overflow math
static AppTimer *s_marq_timer = NULL;

int get_total_connections(const Bundle *b);
bool get_connection_at(const Bundle *b, int conn_idx, uint8_t *out_line, uint8_t *out_dir);
int get_connection_index(const Bundle *b, uint8_t line, uint8_t dir);

static void flip_step(void *ctx);
static void flip_start(void);
static void flip_stop(void);
static void load_start(void);
static void load_stop(void);
static void render_dispatch(void);
static void open_settings(ClickRecognizerRef r, void *c);
static void open_manage(void);
static void open_alerts(void);
static void push_favsync(void);
static void connect_timeout_cb(void *ctx);
static void save_cached_bundle(const uint8_t *data, size_t len);
#if defined(MTA_DEBUG_STUB) || defined(SCREENSHOT_EMU)
static void stub_arrive(void *ctx);
#endif

// Connection watchdog: if no bundle and no error land within CONNECT_TIMEOUT_MS
// of asking, stop spinning and tell the user the phone is unreachable. Armed at
// launch and on every refresh request; cancelled when real data or a definitive
// error arrives. (s_connect_wd lives here so the arm/cancel helpers can see it.)
static AppTimer *s_connect_wd = NULL;
static void connect_wd_arm(void) {
  if (s_connect_wd) app_timer_cancel(s_connect_wd);
  s_connect_wd = app_timer_register(CONNECT_TIMEOUT_MS, connect_timeout_cb, NULL);
}
static void connect_wd_cancel(void) {
  if (s_connect_wd) { app_timer_cancel(s_connect_wd); s_connect_wd = NULL; }
}

// Single source of truth for which screen the hero window is showing, so the
// renderer (canvas_update) and the click handlers never disagree about whether
// there's a live board to drive or an error/offline card to retry from.
typedef enum { SCR_BOARD, SCR_LOADING, SCR_ERROR, SCR_OFFLINE } ScreenKind;
static bool bundle_stale(void) {
  return s_have_bundle && (int)(time(NULL) - (time_t)s_bundle.epochBase) > STALE_SECS;
}
static ScreenKind current_screen(void) {
  if (s_error == 1 || s_error == 2 || s_error == 4 || s_error == 5) return SCR_ERROR;
  if (s_error == 3 && !s_have_bundle)                               return SCR_ERROR;
  if (s_switching || !s_have_bundle || loading_active())            return SCR_LOADING;
  if (bundle_stale())                                               return SCR_OFFLINE;
  return SCR_BOARD;
}

// Service-alerts screen.
static Window     *s_alerts_win;
static ScrollLayer *s_alerts_scroll;
static TextLayer  *s_alerts_text;

// First-run controls card.
static Window    *s_help;
static TextLayer *s_help_title;
static TextLayer *s_help_body;
static TextLayer *s_help_note;

// The ring is favorites in stored order with the Nearest slot inserted at
// s_nearest_pos. Nearest is permanent: it is always present and cannot be
// hidden, so the ring is never empty. (s_nearest_pos holds only a position;
// a legacy 255 "off" value is migrated to 0 on load.)
static bool nearest_on(void) {
  if (favorites_count() == 0) return true;
  return s_nearest_pos != 254;
}
static uint8_t nearest_idx(void) {
  uint8_t p = (s_nearest_pos == 255) ? 0 : s_nearest_pos;
  if (p > favorites_count()) p = favorites_count();
  return p;
}
static uint8_t ring_len(void) {
  return favorites_count() + (nearest_on() ? 1 : 0);
}
static bool ring_is_nearest(uint8_t ring_idx) {
  return nearest_on() && ring_idx == nearest_idx();
}
// Favorite array index for a NON-Nearest ring slot (guard with ring_is_nearest).
static uint8_t ring_fav_index(uint8_t ring_idx) {
  uint8_t ni = nearest_idx();
  if (!nearest_on() || ring_idx < ni) return ring_idx;
  return ring_idx - 1;
}
// Ring index that currently holds favorite j (inverse of ring_fav_index).
static uint8_t fav_ring_index(uint8_t fav_j) {
  uint8_t ni = nearest_idx();
  if (!nearest_on() || fav_j < ni) return fav_j;
  return fav_j + 1;
}
static void clamp_sel(void) {
  uint8_t rl = ring_len();
  if (rl == 0) { s_sel = 0; return; }
  if (s_sel >= rl) s_sel = rl - 1;
}

static bool has_alerts(void) { return s_alerts[0] != '\0'; }

// Settings rows are dynamic, in order: Add, [Manage if favorites],
// [Alerts if active], Refresh.
typedef enum { ROW_ADD, ROW_MANAGE, ROW_ALERTS, ROW_REFRESH, ROW_CLOCK } SettingsRow;
static uint8_t settings_rows(SettingsRow *order) {
  uint8_t n = 0;
  order[n++] = ROW_ADD;
  if (favorites_count() > 0) order[n++] = ROW_MANAGE;
  if (has_alerts())          order[n++] = ROW_ALERTS;
  order[n++] = ROW_REFRESH;
  order[n++] = ROW_CLOCK;
  return n;
}
static SettingsRow settings_row(uint16_t r) {
  SettingsRow order[5];
  uint8_t n = settings_rows(order);
  if (r >= n) r = n - 1;
  return order[r];
}

// AppMessage outbox is single-slot: a second outbox_begin while a send is in
// flight returns APP_MSG_BUSY and drops the message. The launch handshake needs
// to send BOTH a favsync and a refresh request, so serialize them through a tiny
// queue pumped by the outbox_sent/failed callbacks.
static bool s_tx_busy = false;
static bool s_tx_refresh = false;
static bool s_tx_favsync = false;
static bool s_tx_clock = false;   // push the watch's clock mode to the phone (config mirror)
// Per-request token, echoed by the phone so we can drop a stale reply that lands
// after we've already navigated to a different station (the empty no-key agencies
// reply fast and would otherwise clobber a slower bundle). A decimal STRING, to
// dodge JS<->C int-width ambiguity across AppMessage.
static uint32_t s_req_token = 0;

static void tx_pump(void) {
  if (s_tx_busy) return;
  DictionaryIterator *out;
  if (s_tx_refresh) {
    if (app_message_outbox_begin(&out) != APP_MSG_OK) return;  // retry on next sent/failed
    dict_write_uint8(out, MESSAGE_KEY_Request, 1);
    char tok[12];
    snprintf(tok, sizeof(tok), "%u", (unsigned)s_req_token);
    dict_write_cstring(out, MESSAGE_KEY_Req, tok);
    if (s_startup) {
      dict_write_uint8(out, MESSAGE_KEY_UseNearest, (s_nearest_pos > 0) ? 2 : 1);
    } else if (ring_is_nearest(s_sel)) {
      dict_write_uint8(out, MESSAGE_KEY_UseNearest, 1);
    } else {
      const Fav *f = favorites_get(ring_fav_index(s_sel));
      if (f) {
        dict_write_cstring(out, MESSAGE_KEY_StationId, f->id);
        dict_write_cstring(out, MESSAGE_KEY_Agency, f->agency);   // "" for legacy favorites -> id-only resolve
      } else dict_write_uint8(out, MESSAGE_KEY_UseNearest, 1);
    }
    if (app_message_outbox_send() == APP_MSG_OK) { s_tx_refresh = false; s_tx_busy = true; }
    return;
  }
  if (s_tx_favsync) {
    // static (not stack): ~1.1 KB is a lot for a Pebble app stack, and the
    // single-threaded event loop means this is never reentered. Sized for the
    // favsync_encode worst case: header(2) + per favorite four length-prefixed
    // fields (id, name, label, agency) — a length byte plus max payload each.
    static uint8_t buf[2 + FAV_MAX * (4 + (FAV_ID_LEN - 1) + (FAV_NAME_LEN - 1) + (FAV_LABEL_LEN - 1) + (FAV_AGENCY_LEN - 1))];
    size_t n = favsync_encode(s_nearest_pos, buf, sizeof(buf));
    if (!n) { s_tx_favsync = false; return; }
    if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
    dict_write_data(out, MESSAGE_KEY_FavSync, buf, n);
    if (app_message_outbox_send() == APP_MSG_OK) { s_tx_favsync = false; s_tx_busy = true; }
    return;
  }
  if (s_tx_clock) {
    if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
    dict_write_uint8(out, MESSAGE_KEY_Clock, s_clock_mode);
    if (app_message_outbox_send() == APP_MSG_OK) { s_tx_clock = false; s_tx_busy = true; }
    return;
  }
}

static void request_refresh(void) {
#if defined(MTA_DEBUG_STUB) || defined(SCREENSHOT_EMU)
  app_timer_register(1500, stub_arrive, NULL);   // emulate the phone's feed reply
  return;
#endif
  s_req_token++;      // new logical request; any reply tagged with an older token is stale
  connect_wd_arm();   // restart the unreachable-phone clock for this attempt
  s_tx_refresh = true; tx_pump();
}
static void push_favsync(void) { s_tx_favsync = true; tx_pump(); }

// Watchdog fired: nothing came back in time. Stop the spinner and surface an
// honest dead end -- "Can't reach phone" with no cache, or the Offline card if
// we still have (stale) cached data. Either way the user can press to retry.
static void connect_timeout_cb(void *ctx) {
  s_connect_wd = NULL;
  s_switching = false;
  if (loading_active()) { loading_deinit(); load_stop(); }
  if (!s_have_bundle) s_error = 5;   // else fall through to the stale Offline card
  render_dispatch();
}

// Press-to-retry from any error/offline card: ask again and show the spinner.
static void retry_connection(void) {
  s_error = -1;
  s_switching = true;
  connect_wd_arm();
  request_refresh();
  render_dispatch();
}

static void outbox_sent(DictionaryIterator *it, void *ctx) { s_tx_busy = false; tx_pump(); }
static void outbox_failed(DictionaryIterator *it, AppMessageResult r, void *ctx) {
  s_tx_busy = false; tx_pump();
}

static uint16_t menu_num_rows(MenuLayer *m, uint16_t section, void *ctx) {
  SettingsRow order[5];
  return settings_rows(order);
}

static void menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  switch (settings_row(idx->row)) {
    case ROW_ADD: {
      const char *sub;
      if (!s_have_bundle)                              sub = "No station loaded";
      else if (favorites_index_of(s_bundle.id) >= 0)   sub = "Already a favorite";
      else if (favorites_count() >= FAV_MAX)           sub = "Favorites full (10)";
      else                                             sub = s_bundle.station;
      menu_cell_basic_draw(ctx, cell, "Add to favorites", sub, NULL);
      break;
    }
    case ROW_MANAGE: {
      static char cnt[16];
      snprintf(cnt, sizeof(cnt), "%d saved", (int)favorites_count());
      menu_cell_basic_draw(ctx, cell, "Manage favorites", cnt, NULL);
      break;
    }
    case ROW_ALERTS: {
      // Count headlines (one per line) for the subtitle.
      int lines = s_alerts[0] ? 1 : 0;
      for (const char *p = s_alerts; *p; p++) if (*p == '\n') lines++;
      static char sub[20];
      snprintf(sub, sizeof(sub), "%d active", lines);
      menu_cell_basic_draw(ctx, cell, "Service alerts", sub, NULL);
      break;
    }
    case ROW_REFRESH:
      menu_cell_basic_draw(ctx, cell, "Refresh now", NULL, NULL);
      break;
    case ROW_CLOCK: {
      const char *v = s_clock_mode == CLOCK_12H ? "12-hour"
                    : s_clock_mode == CLOCK_24H ? "24-hour" : "Auto";
      menu_cell_basic_draw(ctx, cell, "Clock", v, NULL);
      break;
    }
  }
}

static void menu_select(MenuLayer *m, MenuIndex *idx, void *c) {
  switch (settings_row(idx->row)) {
    case ROW_ADD:
      if (s_have_bundle && favorites_index_of(s_bundle.id) < 0 &&
          favorites_add(s_bundle.id, s_bundle.station)) {
        // Jump the ring to the just-added favorite (the new last entry).
        s_sel = fav_ring_index(favorites_count() - 1);
        persist_write_int(PERSIST_SEL, s_sel);
        push_favsync();
      }
      menu_layer_reload_data(m);
      break;
    case ROW_MANAGE:
      open_manage();
      break;
    case ROW_ALERTS:
      open_alerts();
      break;
    case ROW_REFRESH:
      request_refresh();
      window_stack_pop(true);
      break;
    case ROW_CLOCK:
      s_clock_mode = (s_clock_mode + 1) % 3;     // Auto -> 12h -> 24h -> Auto
      hero_set_clock_mode((ClockMode)s_clock_mode);
      persist_write_int(PERSIST_CLOCK, s_clock_mode);
      s_tx_clock = true; tx_pump();              // keep the phone config mirror in sync
      menu_layer_reload_data(m);
      break;
  }
}

static void settings_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_menu = menu_layer_create(b);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = menu_num_rows,
    .draw_row = menu_draw_row,
    .select_click = menu_select,
  });
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}
static void settings_unload(Window *w) { menu_layer_destroy(s_menu); }

static void confirm_yes(ClickRecognizerRef r, void *c) {
  uint8_t removed = s_confirm_row;                     // favorite array index
  favorites_remove(removed);
  // The Nearest slot collapses by one if it sat after the removed favorite.
  if (s_nearest_pos != 255 && s_nearest_pos > removed) s_nearest_pos--;
  persist_write_int(PERSIST_NEAREST_POS, s_nearest_pos);
  clamp_sel();
  persist_write_int(PERSIST_SEL, s_sel);
  push_favsync();
  s_move_row = -1;
  window_stack_pop(true);                              // pop confirm
  if (favorites_count() == 0 && !nearest_on()) window_stack_pop(true);
  else menu_layer_reload_data(s_manage_menu);
}
static void confirm_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, confirm_yes);
}
static void confirm_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  const Fav *f = favorites_get(s_confirm_row);
  static char msg[72];
  snprintf(msg, sizeof(msg), "Remove\n%s?", f ? f->name : "");
  s_confirm_msg = text_layer_create(GRect(4, b.size.h / 2 - 40, b.size.w - 8, 80));
  text_layer_set_background_color(s_confirm_msg, GColorClear);
  text_layer_set_text_color(s_confirm_msg, GColorWhite);
  text_layer_set_text_alignment(s_confirm_msg, GTextAlignmentCenter);
  text_layer_set_font(s_confirm_msg, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text(s_confirm_msg, msg);
  layer_add_child(root, text_layer_get_layer(s_confirm_msg));
  s_confirm_hint = text_layer_create(GRect(4, b.size.h - 28, b.size.w - 8, 24));
  text_layer_set_background_color(s_confirm_hint, GColorClear);
  text_layer_set_text_color(s_confirm_hint, GColorLightGray);
  text_layer_set_text_alignment(s_confirm_hint, GTextAlignmentCenter);
  text_layer_set_font(s_confirm_hint, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_confirm_hint, "SELECT remove · BACK cancel");
  layer_add_child(root, text_layer_get_layer(s_confirm_hint));
}
static void confirm_unload(Window *w) {
  text_layer_destroy(s_confirm_msg);
  text_layer_destroy(s_confirm_hint);
}

static uint16_t manage_num_rows(MenuLayer *m, uint16_t s, void *c) { return ring_len(); }

// Title and subtitle ("" if none) for a manage ring row. Shared by the row
// renderer and the marquee width calculation so they never disagree.
static void manage_row_text(uint16_t row, const char **title, const char **sub) {
  bool moving = (s_move_row == (int)row);
  if (ring_is_nearest(row)) {
    *title = "Nearest";
    *sub = moving ? "Up/Down move · SELECT drop" : "Auto by GPS";
    return;
  }
  const Fav *f = favorites_get(ring_fav_index(row));
  if (!f) { *title = ""; *sub = ""; return; }
  if (moving)            { *title = f->label[0] ? f->label : f->name; *sub = "Up/Down move · SELECT drop"; }
  else if (f->label[0])  { *title = f->label; *sub = f->name; }
  else                   { *title = f->name; *sub = ""; }
}

static int marq_inset(void) { return PBL_IF_ROUND_ELSE(MARQ_INSET_ROUND, MARQ_INSET_RECT); }

static void marq_tick(void *ctx);
static void marq_schedule(uint32_t ms) {
  if (s_marq_timer) app_timer_cancel(s_marq_timer);
  s_marq_timer = app_timer_register(ms, marq_tick, NULL);
}
static void marq_tick(void *ctx) {
  s_marq_timer = NULL;
  if (s_marq_max <= 0 || s_marq_row < 0) return;
  if (s_marq_x >= s_marq_max) {            // tail fully shown → reset and long pause
    s_marq_x = 0;
    marq_schedule(MARQ_DELAY_MS);
  } else {
    s_marq_x += MARQ_STEP_PX;
    if (s_marq_x >= s_marq_max) { s_marq_x = s_marq_max; marq_schedule(MARQ_END_MS); }
    else                          marq_schedule(MARQ_STEP_MS);
  }
  if (s_manage_menu) layer_mark_dirty(menu_layer_get_layer(s_manage_menu));
}
// Point the marquee at `row`, measure its subtitle overflow, and arm the timer.
static void marq_set_row(int row) {
  s_marq_row = row;
  s_marq_x = 0;
  s_marq_max = 0;
  const char *title, *sub;
  manage_row_text((uint16_t)row, &title, &sub);
  if (sub && sub[0]) {
    int avail = s_manage_w - 2 * marq_inset();
    GSize sz = graphics_text_layout_get_content_size(sub,
                 fonts_get_system_font(FONT_KEY_GOTHIC_18),
                 GRect(0, 0, 2000, 24), GTextOverflowModeFill, GTextAlignmentLeft);
    if (sz.w > avail) s_marq_max = sz.w - avail + 4;   // +4 so the last glyph clears
  }
  marq_schedule(MARQ_DELAY_MS);
}
static void marq_stop(void) {
  if (s_marq_timer) { app_timer_cancel(s_marq_timer); s_marq_timer = NULL; }
  s_marq_row = -1; s_marq_x = 0; s_marq_max = 0;
}

static void manage_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  const char *title, *sub;
  manage_row_text(idx->row, &title, &sub);
  bool scrolling = ((int)idx->row == s_marq_row) && s_marq_max > 0;
  if (!scrolling) {
    menu_cell_basic_draw(ctx, cell, title, (sub && sub[0]) ? sub : NULL, NULL);
    return;
  }
  // Selected row whose subtitle overflows: draw the title normally and slide the
  // subtitle left by s_marq_x. The menu clips drawing to the cell, so the
  // off-cell part of the subtitle is hidden.
  GColor fg = menu_cell_layer_is_highlighted(cell) ? GColorBlack : GColorWhite;
  GRect cb = layer_get_bounds(cell);
  int inset = marq_inset();
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
    GRect(inset, -2, cb.size.w - 2 * inset, 26),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_18),
    GRect(inset - s_marq_x, 22, 2000, 22),
    GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void manage_selection_changed(MenuLayer *m, MenuIndex new_index, MenuIndex old_index, void *ctx) {
  marq_set_row(new_index.row);
}

// Move the ring slot at row by dir (+1 down / -1 up). Updates favorite order or
// s_nearest_pos, persists, and returns the slot's new row. No-op at the ends.
static int ring_move(int row, int dir) {
  int target = row + dir;
  if (target < 0 || target >= (int)ring_len()) return row;
  bool row_is_near = ring_is_nearest((uint8_t)row);
  bool tgt_is_near = ring_is_nearest((uint8_t)target);
  if (row_is_near) {
    s_nearest_pos = (uint8_t)((int)nearest_idx() + dir);
    persist_write_int(PERSIST_NEAREST_POS, s_nearest_pos);
  } else if (tgt_is_near) {
    s_nearest_pos = (uint8_t)((int)nearest_idx() - dir);
    persist_write_int(PERSIST_NEAREST_POS, s_nearest_pos);
  } else {
    favorites_swap(ring_fav_index((uint8_t)row), ring_fav_index((uint8_t)target));
  }
  if (s_sel == (uint8_t)row)         s_sel = (uint8_t)target;
  else if (s_sel == (uint8_t)target) s_sel = (uint8_t)row;
  persist_write_int(PERSIST_SEL, s_sel);
  push_favsync();
  return target;
}

static void manage_up(ClickRecognizerRef r, void *c) {
  if (s_move_row < 0) {
    menu_layer_set_selected_next(s_manage_menu, true, MenuRowAlignCenter, true);
  } else {
    s_move_row = ring_move(s_move_row, -1);
    menu_layer_set_selected_index(s_manage_menu, MenuIndex(0, s_move_row), MenuRowAlignCenter, false);
    menu_layer_reload_data(s_manage_menu);
    marq_set_row(s_move_row);
  }
}
static void manage_down(ClickRecognizerRef r, void *c) {
  if (s_move_row < 0) {
    menu_layer_set_selected_next(s_manage_menu, false, MenuRowAlignCenter, true);
  } else {
    s_move_row = ring_move(s_move_row, +1);
    menu_layer_set_selected_index(s_manage_menu, MenuIndex(0, s_move_row), MenuRowAlignCenter, false);
    menu_layer_reload_data(s_manage_menu);
    marq_set_row(s_move_row);
  }
}
static void manage_select(ClickRecognizerRef r, void *c) {
  MenuIndex idx = menu_layer_get_selected_index(s_manage_menu);
  if (s_move_row < 0) s_move_row = idx.row;   // enter move mode for selected row
  else                s_move_row = -1;        // drop
  menu_layer_reload_data(s_manage_menu);
  marq_set_row(idx.row);                       // subtitle changed with the mode
}
static void manage_remove(ClickRecognizerRef r, void *c) {
  if (s_move_row >= 0) return;                 // no actions mid-move
  MenuIndex idx = menu_layer_get_selected_index(s_manage_menu);
  if (ring_is_nearest(idx.row)) return;        // Nearest is permanent; can't remove
  s_confirm_row = ring_fav_index(idx.row);     // favorite array index to remove
  window_stack_push(s_confirm, true);
}
static void manage_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, manage_up);
  window_single_click_subscribe(BUTTON_ID_DOWN, manage_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, manage_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, manage_remove, NULL);
}

static void manage_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_manage_w = b.size.w;
  s_manage_menu = menu_layer_create(b);
  menu_layer_set_callbacks(s_manage_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = manage_num_rows,
    .draw_row = manage_draw_row,
    .selection_changed = manage_selection_changed,
  });
  menu_layer_set_normal_colors(s_manage_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_manage_menu,
    PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite), GColorBlack);
  layer_add_child(root, menu_layer_get_layer(s_manage_menu));
  // Custom provider (NOT menu_layer_set_click_config_onto_window) so move mode
  // can repurpose Up/Down.
  window_set_click_config_provider_with_context(w, manage_click_config, w);
  marq_set_row(0);   // selection_changed doesn't fire for the initial row
}
static void manage_unload(Window *w) { marq_stop(); menu_layer_destroy(s_manage_menu); }

static void open_manage(void) {
  s_move_row = -1;
  window_stack_push(s_manage, true);
}

static void alerts_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_alerts_scroll = scroll_layer_create(b);
  scroll_layer_set_click_config_onto_window(s_alerts_scroll, w);
  s_alerts_text = text_layer_create(GRect(4, 2, b.size.w - 8, 2000));
  text_layer_set_background_color(s_alerts_text, GColorClear);
  text_layer_set_text_color(s_alerts_text, GColorWhite);
  text_layer_set_font(s_alerts_text, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text(s_alerts_text, s_alerts);
  GSize used = text_layer_get_content_size(s_alerts_text);
  text_layer_set_size(s_alerts_text, GSize(b.size.w - 8, used.h + 8));
  scroll_layer_set_content_size(s_alerts_scroll, GSize(b.size.w, used.h + 16));
  scroll_layer_add_child(s_alerts_scroll, text_layer_get_layer(s_alerts_text));
  layer_add_child(root, scroll_layer_get_layer(s_alerts_scroll));
}
static void alerts_unload(Window *w) {
  text_layer_destroy(s_alerts_text);
  scroll_layer_destroy(s_alerts_scroll);
}
static void open_alerts(void) { window_stack_push(s_alerts_win, true); }

static void help_dismiss(ClickRecognizerRef r, void *c) {
  persist_write_bool(PERSIST_SEEN_HELP, true);
  window_stack_remove(s_help, true);
}
static void help_click_config(void *ctx) {
  // Any button dismisses; BACK is overridden so it also records "seen".
  window_single_click_subscribe(BUTTON_ID_SELECT, help_dismiss);
  window_single_click_subscribe(BUTTON_ID_UP, help_dismiss);
  window_single_click_subscribe(BUTTON_ID_DOWN, help_dismiss);
  window_single_click_subscribe(BUTTON_ID_BACK, help_dismiss);
}
static void help_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  int bx = PBL_IF_ROUND_ELSE(26, 6);
  int top = PBL_IF_ROUND_ELSE(14, 4);

  s_help_title = text_layer_create(GRect(4, top, b.size.w - 8, 28));
  text_layer_set_background_color(s_help_title, GColorClear);
  text_layer_set_text_color(s_help_title, GColorWhite);
  text_layer_set_text_alignment(s_help_title, GTextAlignmentCenter);
  text_layer_set_font(s_help_title, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text(s_help_title, "Controls");
  layer_add_child(root, text_layer_get_layer(s_help_title));

  s_help_body = text_layer_create(GRect(bx, top + 30, b.size.w - 2 * bx, 96));
  text_layer_set_background_color(s_help_body, GColorClear);
  text_layer_set_text_color(s_help_body, GColorWhite);
  text_layer_set_text_alignment(s_help_body, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
  text_layer_set_font(s_help_body, fonts_get_system_font(FONT_KEY_GOTHIC_14));   // 14 both shapes so five rows fit
  text_layer_set_text(s_help_body,
    "UP/DN — Line\n"
    "SELECT — Direction\n"
    "Hold SELECT — Menu\n"
    "Hold UP/DN — Station\n"
    "2x BACK — Alerts");
  layer_add_child(root, text_layer_get_layer(s_help_body));

  int note_top = PBL_IF_ROUND_ELSE(b.size.h - 50, b.size.h - 46);
  s_help_note = text_layer_create(GRect(bx, note_top, b.size.w - 2 * bx, b.size.h - note_top));
  text_layer_set_background_color(s_help_note, GColorClear);
  text_layer_set_text_color(s_help_note, GColorLightGray);
  text_layer_set_text_alignment(s_help_note, GTextAlignmentCenter);
  text_layer_set_font(s_help_note, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_help_note, "Add any station in the Pebble app. Press a button.");
  layer_add_child(root, text_layer_get_layer(s_help_note));
}
static void help_unload(Window *w) {
  text_layer_destroy(s_help_title);
  text_layer_destroy(s_help_body);
  text_layer_destroy(s_help_note);
}

static void open_settings(ClickRecognizerRef r, void *c) {
  window_stack_push(s_settings, true);
}

static void switch_to(uint8_t sel) {
  s_startup = false;
  arrival_cancel();   // a station scroll takes over from any in-flight gold wipe
  uint8_t rl = ring_len();
  if (rl == 0) return;
  s_sel = sel % rl;
  s_line = 0; s_dir = 0;
  memset(s_line_dir, 0, sizeof(s_line_dir));  // new station: per-line directions reset
  s_alerts[0] = '\0';   // old station's alerts no longer apply
  if (ring_is_nearest(s_sel)) snprintf(s_hint, sizeof(s_hint), "Nearest");
  else {
    const Fav *f = favorites_get(ring_fav_index(s_sel));
    snprintf(s_hint, sizeof(s_hint), "%s", f ? f->name : "Nearest");
  }
  s_switching = true;
  s_error = -1;
  if (s_launch_settle) { app_timer_cancel(s_launch_settle); s_launch_settle = NULL; }
  persist_write_int(PERSIST_SEL, s_sel);
  request_refresh();
  render_dispatch();
}
static void ring_next(ClickRecognizerRef r, void *c) { switch_to(s_sel + 1); }
static void ring_prev(ClickRecognizerRef r, void *c) {
  uint8_t rl = ring_len();
  switch_to((uint8_t)((s_sel + rl - 1) % rl));
}

// Per-station view memory: remembers the line+direction last viewed at each
// station, keyed by station id, so returning to a station reopens on the line you
// left it at (rather than always snapping back to the top line). An MRU table of
// the most recent VIEWMEM_MAX stations, persisted whole under PERSIST_VIEWMEM2 so
// it survives app restarts. Covers favorites and the (GPS-varying) Nearest station
// uniformly, since both resolve to a real station id.
#define VIEWMEM_MAX 16
// Entries key on a 32-bit FNV-1a hash of the station id, not the id string:
// full ids run up to 39 bytes (SEPTA), and 16 string-keyed entries would blow
// the 256-byte persist cap. A hash collision merely restores the wrong
// line/dir once — cosmetic.
typedef struct { uint32_t idHash; uint8_t line; uint8_t dir; } ViewMem;
static ViewMem s_viewmem[VIEWMEM_MAX];
static uint8_t s_viewmem_n;

static uint32_t id_hash(const char *id) {
  uint32_t h = 2166136261u;                       // FNV-1a
  for (const char *p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  return h;
}

static void viewmem_load(void) {
  s_viewmem_n = 0;
  // The pre-v8 table (string-keyed entries) lived under PERSIST_VIEWMEM with a
  // different entry size; reading it as the new struct would garble. Drop it
  // and start fresh under PERSIST_VIEWMEM2 — view memory is a cosmetic cache.
  if (persist_exists(PERSIST_VIEWMEM)) persist_delete(PERSIST_VIEWMEM);
  if (!persist_exists(PERSIST_VIEWMEM2)) return;
  int sz = persist_read_data(PERSIST_VIEWMEM2, s_viewmem, sizeof(s_viewmem));
  if (sz <= 0 || sz % (int)sizeof(ViewMem) != 0) return;   // partial/foreign blob: ignore
  int n = sz / (int)sizeof(ViewMem);
  if (n > VIEWMEM_MAX) n = VIEWMEM_MAX;
  s_viewmem_n = (uint8_t)n;
}
// Restore the remembered line+dir for `id` into *line/*dir. Defaults to the top
// line (0,0) when the station has never been seen. Always returns true so callers
// can branch cleanly; values are clamped to the bundle by clamp_view afterward.
static void viewmem_get(const char *id, uint8_t *line, uint8_t *dir) {
  uint32_t h = id_hash(id);
  for (uint8_t i = 0; i < s_viewmem_n; i++) {
    if (s_viewmem[i].idHash == h) {
      *line = s_viewmem[i].line; *dir = s_viewmem[i].dir; return;
    }
  }
  *line = 0; *dir = 0;
}
// Record `id`'s current line+dir at the front of the MRU table and persist.
static void viewmem_put(const char *id, uint8_t line, uint8_t dir) {
  if (!id || !id[0]) return;
  uint32_t h = id_hash(id);
  int at = -1;
  for (uint8_t i = 0; i < s_viewmem_n; i++)
    if (s_viewmem[i].idHash == h) { at = i; break; }
  if (at >= 0) {
    if (s_viewmem[at].line == line && s_viewmem[at].dir == dir && at == 0) return;  // unchanged
  } else {
    at = (s_viewmem_n < VIEWMEM_MAX) ? s_viewmem_n++ : VIEWMEM_MAX - 1;  // append or evict LRU
  }
  // Shift [0..at) down by one and place the entry at the front (MRU).
  for (int i = at; i > 0; i--) s_viewmem[i] = s_viewmem[i - 1];
  s_viewmem[0].idHash = h;
  s_viewmem[0].line = line; s_viewmem[0].dir = dir;
  persist_write_data(PERSIST_VIEWMEM2, s_viewmem, (size_t)s_viewmem_n * sizeof(ViewMem));
}

static void persist_view(void) {
  if (s_line < MAX_LINES) s_line_dir[s_line] = s_dir;  // remember this line's direction
  persist_write_int(PERSIST_VIEW, ((int)s_line << 8) | s_dir);
  if (s_have_bundle) viewmem_put(s_bundle.id, s_line, s_dir);  // per-station memory
}
static void clamp_view(void) {
  if (!s_have_bundle || s_bundle.nLines == 0) { s_line = 0; s_dir = 0; return; }
  if (s_line >= s_bundle.nLines) s_line = 0;
  uint8_t nd = s_bundle.lines[s_line].nDirs;
  if (nd == 0 || s_dir >= nd) s_dir = 0;
}

// Switching lines restores that line's own last-viewed direction, falling back
// to dir 0 when it was never set or the line has fewer directions (e.g. a one-way
// shuttle).
static uint8_t keep_dir(uint8_t to) {
  uint8_t nd = s_bundle.lines[to].nDirs;
  uint8_t d = s_line_dir[to];
  return (nd && d < nd) ? d : 0;
}
// Mid-flip, snap to the in-flight target so the next press flips on from there.
static void snap_active_transition(void) {
  if (!transition_active()) return;
  uint8_t l, d; transition_target(&l, &d);
  s_line = l; s_dir = d;
}
// Mid-load, a line/dir press re-targets the interstitial riffle instead of
// starting a flip the loading screen would hide. The fresh cells re-settle onto
// the new line.
static bool retarget_loading(uint8_t to_line, uint8_t to_dir) {
  if (!loading_active()) return false;
  s_line = to_line; s_dir = to_dir;
  persist_view();
  loading_land(&s_bundle, s_line, s_dir, time(NULL));
  if (s_canvas) layer_mark_dirty(s_canvas);
  return true;
}
// Fast mode: when the user out-presses the flip animation, they're in a rush —
// stop animating and snap straight to the new view. The trigger is "a press
// arrived while a flip was still playing" (you're faster than the animation).
// Once tripped it sticks for FAST_DECAY_MS, re-armed on every fast press, so a
// burst of taps all settle instantly; a pause lets it lapse and the next press
// animates again. Uses an app_timer (reliable) rather than a wall clock.
#define FAST_DECAY_MS 600
static bool s_fast = false;
static AppTimer *s_fast_timer = NULL;

static void fast_expire(void *ctx) { s_fast_timer = NULL; s_fast = false; }
static void fast_arm(void) {
  s_fast = true;
  if (s_fast_timer) app_timer_cancel(s_fast_timer);
  s_fast_timer = app_timer_register(FAST_DECAY_MS, fast_expire, NULL);
}
// Drop any in-flight flip and cut straight to (line,dir) with no animation.
static void commit_view_instant(uint8_t to_line, uint8_t to_dir) {
  flip_stop();
  transition_abort();
  s_line = to_line; s_dir = to_dir;
  persist_view();
  render_dispatch();
}
// Single-line / dead-press nudge: when a press can't change the board (only one
// line, or a one-way line's SELECT), bob just the bullet disc + countdown a few px
// in the pressed direction and spring back — a tactile "nothing here" with no flip,
// leaving the rest of the board still. s_bounce_dy is applied by hero_bounce_band
// in the render pass; the timer animates it out-and-back along a half-sine.
#define BOUNCE_MS   260
#define BOUNCE_AMP  7
static AppTimer *s_bounce_timer = NULL;
static int s_bounce_t = 0;
static int s_bounce_sign = 1;
static int s_bounce_dy = 0;

static void bounce_step(void *ctx) {
  s_bounce_timer = NULL;
  s_bounce_t += 33;
  if (s_bounce_t >= BOUNCE_MS) {                              // home: band back in place
    s_bounce_dy = 0;
    if (s_canvas) layer_mark_dirty(s_canvas);
    return;
  }
  int32_t a = (TRIG_MAX_ANGLE / 2) * s_bounce_t / BOUNCE_MS;  // 0..pi: out and back
  s_bounce_dy = s_bounce_sign * BOUNCE_AMP * sin_lookup(a) / TRIG_MAX_RATIO;
  if (s_canvas) layer_mark_dirty(s_canvas);
  s_bounce_timer = app_timer_register(33, bounce_step, NULL);
}
static void bounce_start(int sign) {
  if (!s_canvas) return;
  if (s_bounce_timer) app_timer_cancel(s_bounce_timer);
  s_bounce_sign = sign;
  s_bounce_t = 0;
  s_bounce_dy = 0;
  s_bounce_timer = app_timer_register(33, bounce_step, NULL);
}

static void next_line(ClickRecognizerRef r, void *c) {
  ScreenKind sk = current_screen();
  if (sk == SCR_ERROR || sk == SCR_OFFLINE) { retry_connection(); return; }
  if (!s_have_bundle) return;
  int tc = get_total_connections(&s_bundle);
  if (tc <= 2 || s_scroll_idx >= tc - 2) { bounce_start(+1); return; }
  s_scroll_idx = s_scroll_idx + 1;
  uint8_t tl = 0, td = 0;
  if (get_connection_at(&s_bundle, s_scroll_idx, &tl, &td)) {
    s_line = tl; s_dir = td;
    persist_view();
  }
  render_dispatch();
}
static void prev_line(ClickRecognizerRef r, void *c) {
  ScreenKind sk = current_screen();
  if (sk == SCR_ERROR || sk == SCR_OFFLINE) { retry_connection(); return; }
  if (!s_have_bundle) return;
  if (s_scroll_idx <= 0) { bounce_start(-1); return; }
  s_scroll_idx = s_scroll_idx - 1;
  uint8_t tl = 0, td = 0;
  if (get_connection_at(&s_bundle, s_scroll_idx, &tl, &td)) {
    s_line = tl; s_dir = td;
    persist_view();
  }
  render_dispatch();
}
static void flip_dir(ClickRecognizerRef r, void *c) {
  ScreenKind sk = current_screen();
  if (sk == SCR_ERROR || sk == SCR_OFFLINE) { retry_connection(); return; }
  bounce_start(+1);
}
static void open_alerts_from_hero(ClickRecognizerRef r, void *c) {
  if (has_alerts()) open_alerts();
}
static void hero_back(ClickRecognizerRef r, void *c) {
  window_stack_pop_all(true);   // single tap BACK exits the app, as normal
}
static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, prev_line);
  window_single_click_subscribe(BUTTON_ID_DOWN, next_line);
  window_single_click_subscribe(BUTTON_ID_SELECT, flip_dir);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, open_settings, NULL);
  window_long_click_subscribe(BUTTON_ID_UP, 0, ring_prev, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 0, ring_next, NULL);
  // The firmware reserves a long press on BACK, but a multi-click is allowed:
  // single tap exits, double tap opens alerts. Claiming BACK with a single-click
  // handler means the exit waits out the double-tap window before firing.
  window_single_click_subscribe(BUTTON_ID_BACK, hero_back);
  window_multi_click_subscribe(BUTTON_ID_BACK, 2, 2, 0, true, open_alerts_from_hero);
}

// Hand the loading interstitial the real values for the freshly-arrived bundle
// so its riffle settles onto the line the user will land on (s_line/s_dir).
static void loading_land_current(void) {
  if (!loading_active() || loading_landing()) return;
  loading_land(&s_bundle, s_line, s_dir, time(NULL));
}

// Launch with a cached bundle flips the board in through the interstitial while a
// fresh fetch is in flight (see init): the riffle plays, and inbox_received lands
// it onto fresh data. This is the fallback — if no fresh data (or error) has
// arrived in time, settle the riffle onto the cached bundle so the board always
// appears, even when the phone is silent.
#define LAUNCH_SETTLE_MS 5000
static void launch_settle_cb(void *ctx) {
  s_launch_settle = NULL;
  if (!s_switching) return;          // fresh data already landed; nothing to rescue
  s_switching = false;
#if defined(PBL_COLOR)
  // Only riffle the cached board in if it's still live; a stale cache resolves to
  // the Offline card instead of animating a dead board on as if it were fresh.
  if (!bundle_stale()) loading_land_current();
  else { loading_deinit(); load_stop(); }
#endif
  render_dispatch();                 // b/w (no riffle): just reveal the cached board
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
#if defined(MTA_DEBUG_STUB) || defined(SCREENSHOT_EMU)
  (void)iter; (void)ctx; return;   // keep the synthetic bundle; ignore the live feed
#endif
  Tuple *alerts = dict_find(iter, MESSAGE_KEY_Alerts);
  if (alerts) {
    if (alerts->type == TUPLE_CSTRING && alerts->length > 1) {
      strncpy(s_alerts, alerts->value->cstring, sizeof(s_alerts) - 1);
      s_alerts[sizeof(s_alerts) - 1] = '\0';
    } else {
      s_alerts[0] = '\0';
    }
    render_dispatch();
    return;
  }
  Tuple *favreq = dict_find(iter, MESSAGE_KEY_FavReq);
  if (favreq) { push_favsync(); s_tx_clock = true; tx_pump(); return; }   // also seed the phone's clock mirror
  Tuple *clk = dict_find(iter, MESSAGE_KEY_Clock);
  if (clk) {                                   // phone config set the clock mode
    uint8_t m = clk->value->uint8;
    if (m <= CLOCK_24H) {
      s_clock_mode = m;
      hero_set_clock_mode((ClockMode)s_clock_mode);
      persist_write_int(PERSIST_CLOCK, s_clock_mode);
      render_dispatch();
    }
    return;
  }
  Tuple *favset = dict_find(iter, MESSAGE_KEY_FavSet);
  if (favset) {
    static Fav items[FAV_MAX];
    uint8_t nearest = 0;
    int n = favsync_decode(favset->value->data, favset->length, items, &nearest);
    if (n >= 0) {
      favorites_replace_all(items, (uint8_t)n);
      s_nearest_pos = (nearest == 255) ? 0 : nearest;   // Nearest is permanent
      persist_write_int(PERSIST_NEAREST_POS, s_nearest_pos);
      clamp_sel();
      persist_write_int(PERSIST_SEL, s_sel);
      push_favsync();
      request_refresh();
      render_dispatch();
    }
    return;
  }
  Tuple *err = dict_find(iter, MESSAGE_KEY_ErrorCode);
  Tuple *bun = dict_find(iter, MESSAGE_KEY_Bundle);
  // Drop a stale station reply: a Req tag that doesn't match the latest request
  // means we navigated away before it arrived (e.g. an empty no-key agency's fast
  // ErrorCode racing a slower bundle). Untagged messages (startup ack) pass.
  Tuple *reqt = dict_find(iter, MESSAGE_KEY_Req);
  if (reqt && (uint32_t)atoi(reqt->value->cstring) != s_req_token) return;
  if (bun) {
    char prev_id[40];
    prev_id[0] = '\0';
    if (s_have_bundle) { strncpy(prev_id, s_bundle.id, sizeof(prev_id) - 1); prev_id[sizeof(prev_id) - 1] = '\0'; }
    if (bundle_decode(bun->value->data, bun->length, &s_bundle)) {
      s_have_bundle = true; s_error = -1;
      connect_wd_cancel();   // live data landed; the phone is reachable
      if (s_launch_settle) { app_timer_cancel(s_launch_settle); s_launch_settle = NULL; }
      if (s_startup) {
        s_startup = false;
        if (s_nearest_pos > 0) {
          int fi = favorites_index_of(s_bundle.id);
          if (fi >= 0) {
            s_sel = fav_ring_index(fi);
            persist_write_int(PERSIST_SEL, s_sel);
            const Fav *f = favorites_get(fi);
            snprintf(s_hint, sizeof(s_hint), "%s", f ? f->name : "Nearest");
          }
        }
      }
      if (strcmp(prev_id, s_bundle.id) != 0) {
        memset(s_line_dir, 0, sizeof(s_line_dir));        // forget the old station's per-line directions
        uint32_t min_delta = 0xFFFFFFFFu;
        uint8_t best_l = 0, best_d = 0;
        for (uint8_t l = 0; l < s_bundle.nLines; l++) {
          for (uint8_t d = 0; d < s_bundle.lines[l].nDirs; d++) {
            if (s_bundle.lines[l].dirs[d].n > 0) {
              uint32_t delta = s_bundle.lines[l].dirs[d].delta[0];
              if (delta < min_delta) { min_delta = delta; best_l = l; best_d = d; }
            }
          }
        }
        s_line = best_l;
        s_dir = best_d;
        if (s_line < MAX_LINES) s_line_dir[s_line] = s_dir;  // seed the restored line's direction
      } else clamp_view();                                // same station refresh: keep the user's view
      s_switching = false;
      save_cached_bundle(bun->value->data, bun->length);
      persist_view();
      s_scroll_idx = get_connection_index(&s_bundle, s_line, s_dir);
      int tc = get_total_connections(&s_bundle);
      int max_scroll = tc - 2;
      if (max_scroll < 0) max_scroll = 0;
      if (s_scroll_idx > max_scroll) s_scroll_idx = max_scroll;
      int fi = favorites_index_of(s_bundle.id);
      if (fi >= 0) favorites_update_name((uint8_t)fi, s_bundle.station);
      loading_land_current();          // settle the interstitial onto real data
    }
  } else if (err) {
    int code = (int)err->value->uint8;
    if (code == 0) {
      if (s_error == 5) s_error = -1;   // phone woke up; drop the watchdog dead end
      push_favsync(); request_refresh();
    } else {
      s_error = code; s_switching = false;
      s_startup = false;
      connect_wd_cancel();   // a verdict arrived; no need for the watchdog screen too
      if (loading_active() && !loading_landing()) { loading_deinit(); load_stop(); }
    }
  }
  render_dispatch();
}

#if !defined(MTA_DEBUG_STUB) && !defined(SCREENSHOT_EMU)
static void load_cached_bundle(void) {
  // One-time cleanup of the legacy single-key cache (it could never hold a
  // multi-line bundle; see PERSIST_BUNDLE_LEN).
  if (persist_exists(PERSIST_BUNDLE)) persist_delete(PERSIST_BUNDLE);
  if (!persist_exists(PERSIST_BUNDLE_LEN)) return;
  int len = persist_read_int(PERSIST_BUNDLE_LEN);
  if (len <= 0 || len > BUNDLE_CACHE_MAX) return;
  uint8_t *buf = malloc((size_t)len);
  if (!buf) return;
  int got = 0;
  for (int c = 0; c < BUNDLE_CHUNKS && got < len; c++) {
    int want = len - got;
    if (want > BUNDLE_CHUNK_SZ) want = BUNDLE_CHUNK_SZ;
    if (persist_read_data(PERSIST_BUNDLE_CHUNK0 + c, buf + got, (size_t)want) != want) break;
    got += want;
  }
  if (got == len && bundle_decode(buf, (size_t)len, &s_bundle)) {
    s_have_bundle = true;
    s_scroll_idx = get_connection_index(&s_bundle, s_line, s_dir);
    int tc = get_total_connections(&s_bundle);
    int max_scroll = tc - 2;
    if (max_scroll < 0) max_scroll = 0;
    if (s_scroll_idx > max_scroll) s_scroll_idx = max_scroll;
  }
  free(buf);
}
#endif

// Persist the raw wire bundle across fixed 256-byte chunk keys (the per-key
// persist cap). The length key is written LAST so a power loss mid-write leaves
// a stale-but-consistent cache (old length + mixed chunks fails bundle_decode's
// bounds checks at worst, never over-reads).
static void save_cached_bundle(const uint8_t *data, size_t len) {
  if (len == 0 || len > BUNDLE_CACHE_MAX) { persist_delete(PERSIST_BUNDLE_LEN); return; }
  size_t off = 0;
  for (int c = 0; c < BUNDLE_CHUNKS; c++) {
    if (off < len) {
      size_t n = len - off;
      if (n > BUNDLE_CHUNK_SZ) n = BUNDLE_CHUNK_SZ;
      persist_write_data(PERSIST_BUNDLE_CHUNK0 + c, data + off, n);
      off += n;
    } else if (persist_exists(PERSIST_BUNDLE_CHUNK0 + c)) {
      persist_delete(PERSIST_BUNDLE_CHUNK0 + c);   // shrink: drop stale tail chunks
    }
  }
  persist_write_int(PERSIST_BUNDLE_LEN, (int)len);
}

#if defined(MTA_DEBUG_STUB) || defined(SCREENSHOT_EMU)
// Synthetic bundle so the hero renders in the emulator, which has no live MTA/PID
// feed. Lets us screenshot and tune the flip/zip without hardware. Compiled in
// only when MTA_DEBUG_STUB or SCREENSHOT_EMU is defined (never in a release build).
static void load_stub_bundle(void) {
  Bundle *B = &s_bundle;
  memset(B, 0, sizeof(*B));
  B->version = 6;
  B->epochBase = (uint32_t)time(NULL);

  uint8_t sel = s_sel;
  if (sel == 0) {
    snprintf(B->station, sizeof(B->station), "Muzeum");
    snprintf(B->id, sizeof(B->id), "Muzeum");
    B->nLines = 2;
    // Line A (Green)
    LineView *La = &B->lines[0];
    snprintf(La->label, sizeof(La->label), "A");
    La->r = 0; La->g = 165; La->b = 98;
    La->nDirs = 2;
    snprintf(La->dirs[0].dest, sizeof(La->dirs[0].dest), "Nemocnice Motol");
    La->dirs[0].dirLabel[0] = '\0';
    La->dirs[0].n = 3; La->dirs[0].delta[0] = 120; La->dirs[0].delta[1] = 420; La->dirs[0].delta[2] = 720;
    snprintf(La->dirs[1].dest, sizeof(La->dirs[1].dest), "Depo Hostivař");
    La->dirs[1].dirLabel[0] = '\0';
    La->dirs[1].n = 3; La->dirs[1].delta[0] = 180; La->dirs[1].delta[1] = 480; La->dirs[1].delta[2] = 780;

    // Line C (Red)
    LineView *Lc = &B->lines[1];
    snprintf(Lc->label, sizeof(Lc->label), "C");
    Lc->r = 217; Lc->g = 34; Lc->b = 42;
    Lc->nDirs = 2;
    snprintf(Lc->dirs[0].dest, sizeof(Lc->dirs[0].dest), "Háje");
    Lc->dirs[0].dirLabel[0] = '\0';
    Lc->dirs[0].n = 3; Lc->dirs[0].delta[0] = 60; Lc->dirs[0].delta[1] = 300; Lc->dirs[0].delta[2] = 540;
    snprintf(Lc->dirs[1].dest, sizeof(Lc->dirs[1].dest), "Letňany");
    Lc->dirs[1].dirLabel[0] = '\0';
    Lc->dirs[1].n = 3; Lc->dirs[1].delta[0] = 240; Lc->dirs[1].delta[1] = 480; Lc->dirs[1].delta[2] = 720;
  } else if (sel == 1) {
    snprintf(B->station, sizeof(B->station), "Hlavní nádraží");
    snprintf(B->id, sizeof(B->id), "Hlavní nádraží");
    B->nLines = 3;
    // Line C (Red)
    LineView *Lc = &B->lines[0];
    snprintf(Lc->label, sizeof(Lc->label), "C");
    Lc->r = 217; Lc->g = 34; Lc->b = 42;
    Lc->nDirs = 2;
    snprintf(Lc->dirs[0].dest, sizeof(Lc->dirs[0].dest), "Háje");
    Lc->dirs[0].dirLabel[0] = '\0';
    Lc->dirs[0].n = 3; Lc->dirs[0].delta[0] = 90; Lc->dirs[0].delta[1] = 330; Lc->dirs[0].delta[2] = 570;
    snprintf(Lc->dirs[1].dest, sizeof(Lc->dirs[1].dest), "Letňany");
    Lc->dirs[1].dirLabel[0] = '\0';
    Lc->dirs[1].n = 3; Lc->dirs[1].delta[0] = 210; Lc->dirs[1].delta[1] = 450; Lc->dirs[1].delta[2] = 690;

    // Tram 9
    LineView *L9 = &B->lines[1];
    snprintf(L9->label, sizeof(L9->label), "9");
    L9->r = 217; L9->g = 34; L9->b = 42;
    L9->nDirs = 2;
    snprintf(L9->dirs[0].dest, sizeof(L9->dirs[0].dest), "Sídliště Řepy");
    L9->dirs[0].dirLabel[0] = '\0';
    L9->dirs[0].n = 3; L9->dirs[0].delta[0] = 180; L9->dirs[0].delta[1] = 540; L9->dirs[0].delta[2] = 900;
    snprintf(L9->dirs[1].dest, sizeof(L9->dirs[1].dest), "Spojovací");
    L9->dirs[1].dirLabel[0] = '\0';
    L9->dirs[1].n = 3; L9->dirs[1].delta[0] = 300; L9->dirs[1].delta[1] = 660; L9->dirs[1].delta[2] = 1020;

    // Tram 26
    LineView *L26 = &B->lines[2];
    snprintf(L26->label, sizeof(L26->label), "26");
    L26->r = 217; L26->g = 34; L26->b = 42;
    L26->nDirs = 2;
    snprintf(L26->dirs[0].dest, sizeof(L26->dirs[0].dest), "Divoká Šárka");
    L26->dirs[0].dirLabel[0] = '\0';
    L26->dirs[0].n = 3; L26->dirs[0].delta[0] = 240; L26->dirs[0].delta[1] = 600; L26->dirs[0].delta[2] = 960;
    snprintf(L26->dirs[1].dest, sizeof(L26->dirs[1].dest), "Hostivař");
    L26->dirs[1].dirLabel[0] = '\0';
    L26->dirs[1].n = 3; L26->dirs[1].delta[0] = 420; L26->dirs[1].delta[1] = 780; L26->dirs[1].delta[2] = 1140;
  } else {
    snprintf(B->station, sizeof(B->station), "Anděl");
    snprintf(B->id, sizeof(B->id), "Anděl");
    B->nLines = 3;
    // Line B (Yellow)
    LineView *Lb = &B->lines[0];
    snprintf(Lb->label, sizeof(Lb->label), "B");
    Lb->r = 250; Lb->g = 197; Lb->b = 0;
    Lb->nDirs = 2;
    snprintf(Lb->dirs[0].dest, sizeof(Lb->dirs[0].dest), "Černý Most");
    Lb->dirs[0].dirLabel[0] = '\0';
    Lb->dirs[0].n = 3; Lb->dirs[0].delta[0] = 120; Lb->dirs[0].delta[1] = 360; Lb->dirs[0].delta[2] = 600;
    snprintf(Lb->dirs[1].dest, sizeof(Lb->dirs[1].dest), "Zličín");
    Lb->dirs[1].dirLabel[0] = '\0';
    Lb->dirs[1].n = 3; Lb->dirs[1].delta[0] = 240; Lb->dirs[1].delta[1] = 480; Lb->dirs[1].delta[2] = 720;

    // Tram 9
    LineView *L9 = &B->lines[1];
    snprintf(L9->label, sizeof(L9->label), "9");
    L9->r = 217; L9->g = 34; L9->b = 42;
    L9->nDirs = 2;
    snprintf(L9->dirs[0].dest, sizeof(L9->dirs[0].dest), "Sídliště Řepy");
    L9->dirs[0].dirLabel[0] = '\0';
    L9->dirs[0].n = 3; L9->dirs[0].delta[0] = 150; L9->dirs[0].delta[1] = 510; L9->dirs[0].delta[2] = 870;
    snprintf(L9->dirs[1].dest, sizeof(L9->dirs[1].dest), "Spojovací");
    L9->dirs[1].dirLabel[0] = '\0';
    L9->dirs[1].n = 3; L9->dirs[1].delta[0] = 270; L9->dirs[1].delta[1] = 630; L9->dirs[1].delta[2] = 990;

    // Tram 10
    LineView *L10 = &B->lines[2];
    snprintf(L10->label, sizeof(L10->label), "10");
    L10->r = 217; L10->g = 34; L10->b = 42;
    L10->nDirs = 2;
    snprintf(L10->dirs[0].dest, sizeof(L10->dirs[0].dest), "Sídliště Řepy");
    L10->dirs[0].dirLabel[0] = '\0';
    L10->dirs[0].n = 3; L10->dirs[0].delta[0] = 180; L10->dirs[0].delta[1] = 660; L10->dirs[0].delta[2] = 1020;
    snprintf(L10->dirs[1].dest, sizeof(L10->dirs[1].dest), "Ďáblice");
    L10->dirs[1].dirLabel[0] = '\0';
    L10->dirs[1].n = 3; L10->dirs[1].delta[0] = 360; L10->dirs[1].delta[1] = 840; L10->dirs[1].delta[2] = 1200;
  }
  s_have_bundle = true;
}

// Emulator stand-in for the phone's feed reply: fills the synthetic bundle and
// lands the loading interstitial so the full riffle->settle can be exercised.
static void stub_arrive(void *ctx) {
  (void)ctx;
  load_stub_bundle();
  s_switching = false; s_error = -1;
  if (s_line >= s_bundle.nLines) { s_line = 0; s_dir = 0; }
  loading_land_current();
  render_dispatch();
}
#endif

// Name of the station the ring is currently on — the selected favorite, or the
// last-loaded station for the Nearest slot. Used so error/loading cards keep a
// station-name footer instead of floating context-free.
static const char *current_station_name(void) {
  if (ring_is_nearest(s_sel)) {
    // Only trust the bundle's name while it's fresh: a stale bundle (e.g. the
    // persisted cache from another city) would caption a Nearest error card
    // with somewhere the rider isn't.
    return (s_have_bundle && !bundle_stale()) ? s_bundle.station : "Nearest";
  }
  const Fav *f = favorites_get(ring_fav_index(s_sel));
  return (f && f->name[0]) ? f->name : (s_have_bundle && !bundle_stale() ? s_bundle.station : "");
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  if (transition_active()) {
    if (transition_render(ctx, b, &s_bundle, time(NULL))) return;
  }

  switch (current_screen()) {
    case SCR_ERROR:
      {
        const char *foot = current_station_name();
        switch (s_error) {
          // "No location" is a GPS failure with no station context, so no footer.
          case 1: states_draw_message(ctx, b, "No location", "Check Location Services, or pick a station", NULL); break;
          case 2: states_draw_message(ctx, b, "No station", "No covered station nearby. Pick a city in the phone app.", NULL); break;
          case 4: states_draw_message(ctx, b, "No trains", "No upcoming arrivals right now", foot); break;
          case 5: states_draw_message(ctx, b, "No phone", "Check Bluetooth, then press to retry", foot); break;
          default: states_draw_message(ctx, b, "No data", "Couldn't load trains - press to retry", foot); break;  // offline, no cache
        }
      }
      return;

    case SCR_LOADING:
      // Departure-board loading interstitial: a ring switch in flight, or a cold
      // start with no data yet. Once the bundle lands the riffle keeps settling
      // (loading_active stays true) until every cell has reached its real value.
#if defined(PBL_COLOR)
      loading_begin(b, ring_is_nearest(s_sel));
      load_start();
      loading_render(ctx, b, time(NULL));
#else  // no Solari interstitial on b/w: a plain status message
      states_draw_message(ctx, b, ring_is_nearest(s_sel) ? "Locating" : "Loading",
                          s_have_bundle ? "Updating arrivals" : "Getting trains",
                          ring_is_nearest(s_sel) ? NULL : current_station_name());
#endif
      return;

    case SCR_OFFLINE:
      // Cached data has gone stale: show an honest offline card instead of a
      // ghosted board whose grey roundel + countdown could pass for a live train.
      states_draw_offline(ctx, b, s_bundle.station,
                          (int)(time(NULL) - (time_t)s_bundle.epochBase) / 60);
      return;

    case SCR_BOARD:
      break;
  }

  hero_draw(ctx, b, &s_bundle, s_line, s_dir, time(NULL));
  arrival_render(ctx, b, &s_bundle, s_line, s_dir, time(NULL));  // gold wipe over the just-drawn board
  if (s_bounce_dy) hero_bounce_band(ctx, b, s_bounce_dy);        // single-line nudge: bob the disc + countdown

  int pos_top = PBL_IF_ROUND_ELSE(20, 2);
#if defined(PBL_ROUND)
  int rr = b.size.w / 2;
  int dy = rr - pos_top - 2;
  int q = rr * rr - dy * dy;
  int half = 0; while ((half + 1) * (half + 1) <= q) half++;
  int pos_inset = rr - half + 2;
#else
  int pos_inset = 4;
#endif

  // Ring position, e.g. "2/4". Shown only when the ring has more than one slot.
  if (ring_len() > 1) {
    static char pos[12];
    snprintf(pos, sizeof(pos), "%d/%d", (int)s_sel + 1, (int)ring_len());
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, pos, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(0, pos_top, b.size.w - pos_inset, 16), GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }

  // Service-alert badge: a warning triangle top-left when the current station
  // has active alerts. Details live behind the settings "Service alerts" row.
  if (s_alerts[0]) {
    int bx = pos_inset;
    int by = pos_top;
    gpath_move_to(s_warn_path, GPoint(bx, by));
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorYellow, GColorWhite));
    gpath_draw_filled(ctx, s_warn_path);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    gpath_draw_outline(ctx, s_warn_path);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, "!", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(bx, by + 1, 16, 15), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }

}
static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_unobstructed_bounds(root);
  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
  transition_init(b);
  window_set_click_config_provider(s_window, click_config);
  s_warn_path = gpath_create(&WARN_TRI);
}
static AppTimer *s_flip_timer = NULL;
static AppTimer *s_arrival_timer = NULL;
static void window_unload(Window *w) {
  if (s_flip_timer) { app_timer_cancel(s_flip_timer); s_flip_timer = NULL; }
  if (s_arrival_timer) { app_timer_cancel(s_arrival_timer); s_arrival_timer = NULL; }
  if (s_fast_timer) { app_timer_cancel(s_fast_timer); s_fast_timer = NULL; }
  if (s_bounce_timer) { app_timer_cancel(s_bounce_timer); s_bounce_timer = NULL; }
  load_stop(); loading_deinit();
  gpath_destroy(s_warn_path); transition_deinit(); layer_destroy(s_canvas); s_canvas = NULL;
}

static void flip_step(void *ctx) {
  s_flip_timer = NULL;
  bool more = transition_step();
  if (s_canvas) layer_mark_dirty(s_canvas);
  if (more) {
    s_flip_timer = app_timer_register(33, flip_step, NULL);
  } else {
    uint8_t l, d;
    transition_target(&l, &d);
    s_line = l; s_dir = d;
    persist_view();
  }
}

static void flip_start(void) {
  if (s_flip_timer) app_timer_cancel(s_flip_timer);
  s_flip_timer = app_timer_register(33, flip_step, NULL);
}

static void flip_stop(void) {
  if (s_flip_timer) { app_timer_cancel(s_flip_timer); s_flip_timer = NULL; }
}

// Loading interstitial timer: drives the riffle while waiting on data and the
// settle once it lands. Stops itself when loading_step reports done.
// Arrival flourish timer: drives the gold station-footer wipe after the load
// settles. The haptic pulse fires once in arrival_begin.
static void arrival_step_cb(void *ctx) {
  s_arrival_timer = NULL;
  bool more = arrival_step();
  if (s_canvas) layer_mark_dirty(s_canvas);
  if (more) s_arrival_timer = app_timer_register(33, arrival_step_cb, NULL);
}
static void arrival_start(bool haptic) {
  arrival_begin(haptic);
  if (!s_arrival_timer) s_arrival_timer = app_timer_register(33, arrival_step_cb, NULL);
}

static AppTimer *s_load_timer = NULL;
static void load_step(void *ctx) {
  s_load_timer = NULL;
  bool was_landing = loading_landing();      // distinguish a real settle from an abort
  bool more = loading_step();
  if (s_canvas) layer_mark_dirty(s_canvas);
  if (more) s_load_timer = app_timer_register(33, load_step, NULL);
  else if (was_landing) arrival_start(true);     // loading just settled onto the board
}
static void load_start(void) {
  if (!s_load_timer) s_load_timer = app_timer_register(33, load_step, NULL);
}
static void load_stop(void) {
  if (s_load_timer) { app_timer_cancel(s_load_timer); s_load_timer = NULL; }
}

static void render_dispatch(void) { if (s_canvas) layer_mark_dirty(s_canvas); }

// Seconds until the displayed line/dir's soonest train, plus an identity key for
// that train (station + line + dir, deliberately NOT the feed time, so the key
// survives refreshes and a countdown can be tracked across them). Returns false
// when there's no live arrival to track (suspended line, empty dir).
static bool view_nearest(int *secs, uint32_t *key) {
  if (!s_have_bundle || s_line >= s_bundle.nLines) return false;
  const LineView *L = &s_bundle.lines[s_line];
  if (L->nDirs == 0 || s_dir >= L->nDirs) return false;
  const DirView *D = &L->dirs[s_dir];
  if (D->n == 0) return false;
  *secs = (int)(s_bundle.epochBase + D->delta[0]) - (int)time(NULL);
  *key = (id_hash(s_bundle.id) & 0xFFFF0000u) | ((uint32_t)s_line << 8) | s_dir;
  return true;
}

// Identity + future-ness of the train we're watching tick down, so we can fire
// the gold wipe exactly once at the moment its countdown reaches "Now".
static uint32_t s_now_key = 0xFFFFFFFFu;
static bool     s_now_future = false;

static void tick_handler(struct tm *t, TimeUnits u) {
  int secs; uint32_t key;
  if (view_nearest(&secs, &key)) {
    bool is_now = (secs / 60) <= 0;               // matches hero's fmt_count threshold
    bool stale = (int)(time(NULL) - (time_t)s_bundle.epochBase) > STALE_SECS;
    // Fire only on a genuine future->Now crossing of the same train, and only
    // when the hero board is actually on screen and the data is fresh.
    if (key == s_now_key && s_now_future && is_now && !stale
        && !loading_active() && !transition_active() && !arrival_active()) {
      arrival_start(false);                        // wipe only — no buzz
    }
    s_now_key = key;
    s_now_future = !is_now;
  } else {
    s_now_key = 0xFFFFFFFFu;
    s_now_future = false;
  }
  if (transition_active()) return;                 // step timer owns redraws mid-flip
  render_dispatch();
}
static void poll_cb(void *ctx) { request_refresh(); s_poll = app_timer_register(30000, poll_cb, NULL); }

// AppGlance: on exit, show the soonest upcoming train for the last-viewed
// station in the launcher tile. The slice expires at that train's departure so
// the launcher clears it automatically. Baked minutes are as-of-exit (a glance
// is approximate by nature); the expiration keeps it from outliving the train.
static char   s_glance_buf[64];
static time_t s_glance_exp;
static void glance_reload_cb(AppGlanceReloadSession *session, size_t limit, void *ctx) {
  if (limit < 1) return;
  AppGlanceSlice slice = {
    .layout = { .icon = APP_GLANCE_SLICE_DEFAULT_ICON, .subtitle_template_string = s_glance_buf },
    .expiration_time = s_glance_exp,
  };
  app_glance_add_slice(session, slice);
}
static void publish_glance(void) {
  if (!s_have_bundle) { app_glance_reload(NULL, NULL); return; }
  time_t now = time(NULL), best = 0;
  const char *blabel = NULL;
  for (uint8_t li = 0; li < s_bundle.nLines; li++) {
    const LineView *L = &s_bundle.lines[li];
    for (uint8_t di = 0; di < L->nDirs; di++) {
      const DirView *D = &L->dirs[di];
      for (uint8_t a = 0; a < D->n; a++) {
        time_t t = (time_t)s_bundle.epochBase + D->delta[a];
        if (t <= now) continue;
        if (best == 0 || t < best) { best = t; blabel = L->label; }
      }
    }
  }
  if (best == 0) { app_glance_reload(NULL, NULL); return; }
  int mins = (int)(best - now) / 60;
  char stn[40];
  hero_station_strip(s_bundle.station, stn, sizeof stn);   // drop the "(lines)" list
  if (mins <= 0) snprintf(s_glance_buf, sizeof(s_glance_buf), "%s · Now — %s", blabel, stn);
  else           snprintf(s_glance_buf, sizeof(s_glance_buf), "%s · %d min — %s", blabel, mins, stn);
  s_glance_exp = best;
  app_glance_reload(glance_reload_cb, NULL);
}

int get_total_connections(const Bundle *b) {
  int count = 0;
  for (uint8_t l = 0; l < b->nLines; l++) {
    const LineView *L = &b->lines[l];
    int dirs = L->nDirs;
    if (dirs == 0) dirs = 1;
    count += dirs;
  }
  return count;
}

bool get_connection_at(const Bundle *b, int conn_idx, uint8_t *out_line, uint8_t *out_dir) {
  int curr = 0;
  for (uint8_t l = 0; l < b->nLines; l++) {
    const LineView *L = &b->lines[l];
    int dirs = L->nDirs;
    if (dirs == 0) {
      if (curr == conn_idx) {
        *out_line = l;
        *out_dir = 0;
        return true;
      }
      curr++;
    } else {
      for (uint8_t d = 0; d < dirs; d++) {
        if (curr == conn_idx) {
          *out_line = l;
          *out_dir = d;
          return true;
        }
        curr++;
      }
    }
  }
  return false;
}

int get_connection_index(const Bundle *b, uint8_t line, uint8_t dir) {
  int curr = 0;
  for (uint8_t l = 0; l < b->nLines; l++) {
    const LineView *L = &b->lines[l];
    int dirs = L->nDirs;
    if (dirs == 0) {
      if (l == line) return curr;
      curr++;
    } else {
      for (uint8_t d = 0; d < dirs; d++) {
        if (l == line && d == dir) return curr;
        curr++;
      }
    }
  }
  return 0;
}

static void init(void) {
  favorites_load();
  viewmem_load();
#ifdef SCREENSHOT_EMU
  s_nearest_pos = 254; // Turn off Nearest slot for screenshots so 1st favorite is focused
  // Clear any old persisted bundle so stale US data from prior runs is purged
  for (int k = 0; k < BUNDLE_CHUNKS; k++) {
    if (persist_exists(PERSIST_BUNDLE_CHUNK0 + k)) persist_delete(PERSIST_BUNDLE_CHUNK0 + k);
  }
  if (persist_exists(PERSIST_BUNDLE_LEN)) persist_delete(PERSIST_BUNDLE_LEN);
  if (persist_exists(PERSIST_BUNDLE)) persist_delete(PERSIST_BUNDLE);
  s_have_bundle = false;
#else
  s_nearest_pos = persist_exists(PERSIST_NEAREST_POS)
    ? (uint8_t)persist_read_int(PERSIST_NEAREST_POS) : 0;
  if (s_nearest_pos == 255) {                  // migrate legacy "off" -> permanent on
    s_nearest_pos = 0;
    persist_write_int(PERSIST_NEAREST_POS, s_nearest_pos);
  }
#endif
  s_clock_mode = persist_exists(PERSIST_CLOCK) ? (uint8_t)persist_read_int(PERSIST_CLOCK) : CLOCK_AUTO;
  hero_set_clock_mode((ClockMode)s_clock_mode);
#if defined(MTA_DEBUG_STUB) || defined(SCREENSHOT_EMU)
  // Cold-start into the loading interstitial, then simulate a feed arrival so
  // the riffle settles — there is no phone to deliver data in the emulator.
  app_timer_register(1500, stub_arrive, NULL);
#else
  load_cached_bundle();
#endif
  if (s_nearest_pos == 0) {
    s_sel = nearest_idx();
    if (s_have_bundle) {
      int fi = favorites_index_of(s_bundle.id);
      if (fi >= 0) {
        s_have_bundle = false;
      }
    }
  } else {
    int fi = s_have_bundle ? favorites_index_of(s_bundle.id) : -1;
    if (fi >= 0) {
      s_sel = fav_ring_index(fi);
    } else {
      s_sel = 0;
      s_have_bundle = false;
    }
  }
  clamp_sel();
  if (persist_exists(PERSIST_VIEW)) {
    int v = persist_read_int(PERSIST_VIEW);
    s_line = (uint8_t)((v >> 8) & 0xff);
    s_dir = (uint8_t)(v & 0xff);
    clamp_view();
  }
  if (s_have_bundle) {
    s_scroll_idx = get_connection_index(&s_bundle, s_line, s_dir);
    int tc = get_total_connections(&s_bundle);
    int max_scroll = tc - 2;
    if (max_scroll < 0) max_scroll = 0;
    if (s_scroll_idx > max_scroll) s_scroll_idx = max_scroll;
  }
  // Reopen with cached data: flip the board in through the interstitial as fresh
  // data fetches, instead of snapping the stale cache on screen. The 'ready'
  // round-trip requests a refresh; inbox_received lands the riffle on arrival,
  // and launch_settle_cb falls back to the cached board if nothing comes.
  if (s_have_bundle) {
    s_switching = true;
    s_launch_settle = app_timer_register(LAUNCH_SETTLE_MS, launch_settle_cb, NULL);
  }
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);

  s_settings = window_create();
  window_set_window_handlers(s_settings, (WindowHandlers){ .load = settings_load, .unload = settings_unload });

  s_manage = window_create();
  window_set_background_color(s_manage, GColorBlack);
  window_set_window_handlers(s_manage, (WindowHandlers){ .load = manage_load, .unload = manage_unload });

  s_confirm = window_create();
  window_set_background_color(s_confirm, GColorBlack);
  window_set_window_handlers(s_confirm, (WindowHandlers){ .load = confirm_load, .unload = confirm_unload });
  window_set_click_config_provider(s_confirm, confirm_click_config);

  s_alerts_win = window_create();
  window_set_background_color(s_alerts_win, GColorBlack);
  window_set_window_handlers(s_alerts_win, (WindowHandlers){ .load = alerts_load, .unload = alerts_unload });

  s_help = window_create();
  window_set_background_color(s_help, GColorBlack);
  window_set_window_handlers(s_help, (WindowHandlers){ .load = help_load, .unload = help_unload });
  window_set_click_config_provider(s_help, help_click_config);

  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  // Right-size the AppMessage buffers instead of asking for the 8200-byte maximum
  // each (~16 KB of heap). The largest inbound payload is a Bundle (<=844 B, v8),
  // an Alerts string (<=700 B), or a FavSet blob (<=1242 B); the largest outbound
  // is a FavSync blob (<=1242 B). Reserving the full maximum starved the flip
  // animation on emery (200x228 → large per-glyph cell buffers), exhausting the
  // 78 KB app heap and faulting on big complex stations. 2 KB / 1.25 KB leaves
  // generous framing headroom while freeing ~13 KB.
  app_message_open(2048, 1408);

  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  s_poll = app_timer_register(30000, poll_cb, NULL);
#if !defined(MTA_DEBUG_STUB) && !defined(SCREENSHOT_EMU)
  connect_wd_arm();   // a silent phone (slow JS / disconnected) must not spin forever
#endif

  // First launch: show the controls card on top of the hero.
  if (!persist_read_bool(PERSIST_SEEN_HELP)) window_stack_push(s_help, true);
}
static void deinit(void) {
  publish_glance();
  tick_timer_service_unsubscribe();
  if (s_poll) app_timer_cancel(s_poll);
  if (s_launch_settle) { app_timer_cancel(s_launch_settle); s_launch_settle = NULL; }
  connect_wd_cancel();
  window_destroy(s_window);
  window_destroy(s_settings);
  window_destroy(s_manage);
  window_destroy(s_confirm);
  window_destroy(s_alerts_win);
  window_destroy(s_help);
}
int main(void) { init(); app_event_loop(); deinit(); }
