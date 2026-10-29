#include "loading.h"
#include "flip.h"
#include "hero.h"
#include <string.h>
#include <stdlib.h>

#define STEP_MS    33      // tick cadence (matches the flip/loading timer)
#define FLIP_MS    120     // one text riffle fold: quick, but a readable flap
#define DISC_MS    150     // one disc roundel fold
#define DOT_MS     350     // animated-dot cadence on the status line

#define LAND_BASE  220     // ms after data arrives before the first cell settles
#define CELL_GAP   26      // ms between successive cells in the settle cascade

#define RIFFLE_N   7       // built-in bullets the disc riffles through (idx 0..6)
#define LAND_LINE  7       // built-in slot reserved for the landed real bullet
#define MAXCELLS   56      // hero_glyphs (<=40) + station footer (<=16)

#if defined(PBL_COLOR)

static const char POOL_ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char POOL_ALNUM[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static const char POOL_DIGIT[] = "0123456789";

typedef struct {
  FlipSlot    slot;
  GRect       cell;
  GFont       font;
  GColor      fg, bg;
  bool        is_disc;
  uint8_t     row;          // hero reading-order row (0=top); used for the pool
  const char *pool;         // text riffle pool (NULL for disc)
  int         pool_n;
  char        cur;          // text: glyph currently shown / folding in
  uint8_t     cur_line;     // disc: built-in line index currently shown
  bool        has_target;   // a real landing value has been assigned
  char        target;       // text landing glyph
  uint8_t     target_line;  // disc landing index (always LAND_LINE)
  int         land_at;      // s_elapsed at which to begin the final settle fold
  bool        final;        // the current fold is the settle onto target
  bool        settled;      // final fold done -> draw static
} LoadCell;

static GRect    s_bounds;
static bool     s_locating;
static bool     s_active;
static bool     s_landing;
static int      s_elapsed;
static LoadCell s_cells[MAXCELLS];
static int      s_ncells;
static Bundle   s_builtin;        // riffle bullets (0..6) + landed bullet (7),
                                  // and line 0 drives the cold-start layout

static int rnd(int n) { return n > 0 ? (rand() % n) : 0; }

// A synthetic bundle whose line 0 produces a full, representative hero layout for
// the cold-start riffle (a 7-char headsign, a 2-digit countdown, two NEXT tokens)
// and whose lines 0..6 carry distinct MTA roundel colors for the disc to riffle
// through. dir=0 reserves the borough row so the headsign sits at its normal
// position; we draw LOADING there rather than riffling a real direction word.
static void seed_builtin(void) {
  static const struct { const char *l; uint8_t r, g, b; } BT[RIFFLE_N] = {
    { "A", 0, 165, 98 },   // PID Metro A (Green)
    { "B", 250, 197, 0 },  // PID Metro B (Yellow)
    { "C", 217, 34, 42 },  // PID Metro C (Red)
    { "9", 217, 34, 42 },  // PID Tram 9 (Red)
    { "22", 217, 34, 42 }, // PID Tram 22 (Red)
    { "S", 0, 114, 198 },  // PID Train S (Blue)
    { "119", 0, 114, 198 } // PID Bus 119 (Blue)
  };
  memset(&s_builtin, 0, sizeof(s_builtin));
  s_builtin.nLines = LAND_LINE + 1;
  s_builtin.epochBase = 0;
  for (int i = 0; i < RIFFLE_N; i++) {
    LineView *L = &s_builtin.lines[i];
    strncpy(L->label, BT[i].l, sizeof(L->label) - 1);
    L->r = BT[i].r; L->g = BT[i].g; L->b = BT[i].b;
  }
  s_builtin.lines[LAND_LINE] = s_builtin.lines[0];   // placeholder until landing
  strncpy(s_builtin.station, "PID", sizeof(s_builtin.station) - 1);

  LineView *L0 = &s_builtin.lines[0];
  L0->nDirs = 1;
  DirView *D = &L0->dirs[0];
  strncpy(D->dest, "LOADING", sizeof(D->dest) - 1);
  // A space (not empty) direction label: keeps row 0 reserved so the riffling
  // headsign sits at its normal lower position, leaving the row-0 band clear for
  // the LOADING status text. An empty label rides the headsign up under LOADING
  // (hero_glyphs: hdr_top = dir_top + (dlabel ? 14 : 2)); the space draws nothing
  // and the row is skipped during the cold riffle anyway.
  strcpy(D->dirLabel, " "); D->n = 3; D->expMask = 0;
  D->delta[0] = 12 * 60; D->delta[1] = 5 * 60; D->delta[2] = 9 * 60;
}

static const char *pool_for_row(uint8_t row, int *n) {
  if (row == 0)              { *n = (int)strlen(POOL_ALPHA); return POOL_ALPHA; }
  if (row == 1 || row == 4)  { *n = (int)strlen(POOL_ALNUM); return POOL_ALNUM; }
  *n = (int)strlen(POOL_DIGIT); return POOL_DIGIT;   // countdown + NEXT digits
}

static void reseed(LoadCell *c) {
  flipslot_free(&c->slot);
  if (c->is_disc) {
    uint8_t ni;
    do { ni = (uint8_t)rnd(RIFFLE_N); } while (ni == c->cur_line);
    uint8_t seq[2] = { c->cur_line, ni };
    flipslot_disc_riffle(&c->slot, c->cell, &s_builtin, seq, 2, s_elapsed, DISC_MS);
    c->cur_line = ni;
  } else {
    char ng;
    do { ng = c->pool[rnd(c->pool_n)]; } while (ng == c->cur);
    flipslot_text_direct(&c->slot, c->cell, c->font, c->fg, c->bg,
                         c->cur, ng, s_elapsed, FLIP_MS);
    c->cur = ng;
  }
}

static void begin_final(LoadCell *c) {
  flipslot_free(&c->slot);
  if (c->is_disc) {
    // Riffle through a few more built-in roundels, then land on the real bullet.
    uint8_t seq[5] = { c->cur_line, (uint8_t)rnd(RIFFLE_N), (uint8_t)rnd(RIFFLE_N),
                       (uint8_t)rnd(RIFFLE_N), LAND_LINE };
    flipslot_disc_riffle(&c->slot, c->cell, &s_builtin, seq, 5, s_elapsed, DISC_MS);
    c->cur_line = LAND_LINE;
  } else {
    flipslot_text_direct(&c->slot, c->cell, c->font, c->fg, c->bg,
                         c->cur, c->target, s_elapsed, FLIP_MS);
    c->cur = c->target;
  }
  c->final = true;
}

// Rebuild the cell array from a bundle's hero layout. `final` false is the
// cold-start riffle (no targets, the borough row is skipped for the LOADING
// status); `final` true assigns each cell its real landing value plus a
// reading-order settle stagger, so the riffle resolves straight into the board.
static void build_cells(const Bundle *b, uint8_t line, uint8_t dir,
                        time_t now, bool final) {
  for (int i = 0; i < s_ncells; i++) flipslot_free(&s_cells[i].slot);
  s_ncells = 0;

  static HeroGlyph hg[HERO_MAX_GLYPHS];   // static: keep these off the small stack
  static HeroGlyph sg[16];
  int nhg = hero_glyphs(s_bounds, b, line, dir, now, hg, HERO_MAX_GLYPHS);
  int nsg = hero_station_glyphs(s_bounds, b->station, sg, 16);

  int order = 0;
  for (int pass = 0; pass < 2; pass++) {
    HeroGlyph *arr = pass ? sg : hg;
    int cnt = pass ? nsg : nhg;
    for (int i = 0; i < cnt && s_ncells < MAXCELLS; i++) {
      HeroGlyph *g = &arr[i];
      if (!final && g->row == 0) continue;       // LOADING is drawn over this row
      LoadCell *c = &s_cells[s_ncells++];
      memset(c, 0, sizeof(*c));
      c->cell = g->cell;
      c->font = g->font;
      c->fg = g->fg; c->bg = g->bg;
      c->row = g->row;
      c->is_disc = (g->kind == HG_DISC);
      c->pool = pool_for_row(g->row, &c->pool_n);
      c->cur = c->is_disc ? 0 : c->pool[rnd(c->pool_n)];
      c->cur_line = (uint8_t)rnd(RIFFLE_N);
      if (final) {
        c->has_target = true;
        if (c->is_disc) c->target_line = LAND_LINE;
        else            c->target = g->ch;
        c->land_at = s_elapsed + LAND_BASE + order * CELL_GAP;
        order++;
      }
    }
  }
}

void loading_begin(GRect bounds, bool locating) {
  if (s_active && s_bounds.size.w == bounds.size.w &&
      s_bounds.size.h == bounds.size.h) {
    s_locating = locating;        // word can change without a rebuild
    return;
  }
  loading_deinit();
  s_bounds = bounds;
  s_locating = locating;
  s_elapsed = 0;
  s_landing = false;
  s_active = true;
  seed_builtin();
  build_cells(&s_builtin, 0, 0, 0, false);
}

void loading_land(const Bundle *b, uint8_t line, uint8_t dir, time_t now) {
  if (!s_active) return;
  if (!b || line >= b->nLines) { loading_deinit(); return; }
  // Re-landing (a line/dir change mid-settle) rebuilds the layout for the new
  // target; the fresh cells riffle and re-settle from wherever they were.

  // Copy the real bullet into the reserved slot so the disc can riffle through
  // built-in roundels and land on the true line's colors/label.
  const LineView *src = &b->lines[line];
  LineView *dst = &s_builtin.lines[LAND_LINE];
  memset(dst, 0, sizeof(*dst));
  memcpy(dst->label, src->label, sizeof(dst->label));
  dst->r = src->r; dst->g = src->g; dst->b = src->b;

  build_cells(b, line, dir, now, true);
  s_landing = true;
}

bool loading_step(void) {
  if (!s_active) return false;
  s_elapsed += STEP_MS;

  bool any_unsettled = false;
  for (int i = 0; i < s_ncells; i++) {
    LoadCell *c = &s_cells[i];
    if (c->settled) continue;
    if (c->slot.ok && s_elapsed < flipslot_duration(&c->slot)) {
      any_unsettled = true; continue;            // fold still animating
    }
    if (c->final) { flipslot_free(&c->slot); c->settled = true; continue; }
    if (c->has_target && s_elapsed >= c->land_at) begin_final(c);
    else                                          reseed(c);
    // Low heap: the fold's scratch alloc failed (slot.ok false). Stop flipping
    // this cell and let it draw static — otherwise it re-mallocs at ~30 fps
    // forever and the riffle never terminates. begin_final/reseed already set
    // c->cur to the value draw_static should show.
    if (!c->slot.ok) { c->settled = true; continue; }
    any_unsettled = true;
  }

  if (s_landing && !any_unsettled) {
    loading_deinit();           // settled: release all scratch before the board shows
    return false;
  }
  return true;
}

bool loading_active(void)  { return s_active; }
bool loading_landing(void) { return s_landing; }

void loading_deinit(void) {
  for (int i = 0; i < s_ncells; i++) flipslot_free(&s_cells[i].slot);
  s_ncells = 0;
  s_active = false;
  s_landing = false;
}

static void draw_status(GContext *ctx) {
  float SY = s_bounds.size.h / 168.0f;
  int dir_top = (int)(6 * SY);
  const char *word = s_locating ? "LOCATING" : "LOADING";
  int dots = (s_elapsed / DOT_MS) % 4;          // 0..3 lit, fixed-width field
  char buf[16];
  snprintf(buf, sizeof(buf), "%s%c%c%c", word,
           dots >= 1 ? '.' : ' ', dots >= 2 ? '.' : ' ', dots >= 3 ? '.' : ' ');
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, dir_top, s_bounds.size.w - 8, 16),
    GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void draw_static(GContext *ctx, LoadCell *c) {
  if (c->is_disc) {
    hero_draw_bullet(ctx, c->cell, &s_builtin, c->cur_line);
    return;
  }
  graphics_context_set_fill_color(ctx, c->bg);
  graphics_fill_rect(ctx, c->cell, 0, GCornerNone);
  if (c->cur == 0 || c->cur == ' ') return;
  char str[2] = { c->cur, 0 };
  graphics_context_set_text_color(ctx, c->fg);
  graphics_draw_text(ctx, str, c->font, c->cell,
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

void loading_render(GContext *ctx, GRect bounds, time_t now) {
  (void)now;
  if (!s_active) return;
  s_bounds = bounds;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  for (int i = 0; i < s_ncells; i++) {
    LoadCell *c = &s_cells[i];
    if (!c->settled && c->slot.ok && s_elapsed < flipslot_duration(&c->slot))
      flipslot_render(&c->slot, ctx, s_elapsed);
    else
      draw_static(ctx, c);
  }

  if (!s_landing) draw_status(ctx);
}

#else  // b/w: no interstitial; caller shows its plain text message.

void loading_begin(GRect bounds, bool locating) { (void)bounds; (void)locating; }
void loading_land(const Bundle *b, uint8_t line, uint8_t dir, time_t now) {
  (void)b; (void)line; (void)dir; (void)now;
}
bool loading_step(void)    { return false; }
bool loading_active(void)  { return false; }
bool loading_landing(void) { return false; }
void loading_deinit(void)  {}
void loading_render(GContext *ctx, GRect bounds, time_t now) {
  (void)ctx; (void)bounds; (void)now;
}

#endif
