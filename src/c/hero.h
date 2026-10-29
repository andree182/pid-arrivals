#pragma once
#include <pebble.h>
#include "bundle.h"

// Clock display mode, persisted on the watch. AUTO follows clock_is_24h_style().
typedef enum { CLOCK_AUTO = 0, CLOCK_12H = 1, CLOCK_24H = 2 } ClockMode;

// Set the footer clock format the hero draws with. Module state (not a hero_draw
// param) so every draw path — including the transition/flip frames that call
// hero_draw without access to the persisted setting — stays in sync. main.c calls
// this on launch and whenever the Clock setting changes.
void hero_set_clock_mode(ClockMode mode);

// Write the current local time into out per the active mode. 12h drops the leading
// zero ("7:42"); 24h is zero-padded ("19:42"). out must hold >= 6 bytes.
void hero_clock_string(char *out, size_t n, ClockMode mode);
int get_total_connections(const Bundle *b);
bool get_connection_at(const Bundle *b, int conn_idx, uint8_t *out_line, uint8_t *out_dir);
int hero_visible_connections(GRect bounds);

// Draws the arrivals hero for lines[line].dirs[dir] into ctx over `bounds`.
// `now` is current epoch secs; countdown = epochBase + delta - now.
void hero_draw(GContext *ctx, GRect bounds, const Bundle *b, uint8_t line, uint8_t dir, time_t now);

// Write `full` into out[n] with the trailing " (lines)" service-line list removed
// (e.g. "14 St-Union Sq (456LNQRW)" -> "14 St-Union Sq"). The phone appends that
// list to every station name; on the watch the line is already the big roundel, so
// it is redundant. A " · PATH" tag and names with no trailing "(...)" are kept.
void hero_station_strip(const char *full, char *out, size_t n);

// The full-width middle band (bullet disc + countdown) for a given canvas
// bounds. This is the region the line-switch flip folds. Height is even.
GRect hero_flip_rect(GRect bounds);

// Draw line `line`'s bullet (plain circle/pill, never express) centered in `cell`
// over a black fill. The flip riffles the disc through real roundels by rendering
// intermediate lines into its scratch cell. Disc geometry is derived from `cell`.
void hero_draw_bullet(GContext *ctx, GRect cell, const Bundle *b, uint8_t line);

// The stack of horizontal bands the line-switch flip folds, top to bottom:
// direction label, headsign, disc+countdown, NEXT row. They tile contiguously
// (each band's bottom is the next band's top) so nothing overlaps on any
// platform. Fills `out` (up to `max`) and returns the count. Each height is even.
#define HERO_FLIP_BANDS 4
int hero_flip_bands(GRect bounds, GRect *out, int max);

// One flip-able glyph of the hero: a single character (countdown digit, headsign
// letter, direction-label letter) or the line disc. The split-flap transition
// riffles each text glyph forward through its ring (0-9 / A-Z) from the OLD
// line's char to the NEW line's char; the disc folds once as a pixel block.
typedef enum {
  HG_TEXT,   // a character at `ch`, rendered with `font`/`fg` on `bg` — riffles
  HG_DISC,   // the line bullet — captured as pixels and single-folded, not a char
} HeroGlyphKind;

typedef struct {
  GRect         cell;     // pixel rect within bounds (one glyph's box)
  char          ch;       // the character (HG_TEXT); 0 for HG_DISC
  GFont         font;     // face to render intermediate chars (HG_TEXT)
  GColor        fg, bg;   // ink and background
  uint8_t       row;      // reading-order row index (0=top) for the cascade
  HeroGlyphKind kind;
} HeroGlyph;

// Describe (do not draw) the flip-region glyphs for lines[line].dirs[dir] in
// reading order (top band first, left-to-right within a band). Mirrors the exact
// positions hero_draw paints, so the settled flip frame lands pixel-on-top of the
// static hero. Wrapped text (e.g. the round headsign) is skipped. Fills `out` up
// to `max` and returns the count.
#define HERO_MAX_GLYPHS 40
int hero_glyphs(GRect bounds, const Bundle *b, uint8_t line, uint8_t dir,
                time_t now, HeroGlyph *out, int max);

// Emit the station-footer glyphs (row 4) at the exact position hero_draw paints
// the station name: GOTHIC_14, dark-gray, centered along the bottom. A name that
// wraps to two lines emits nothing (matching hero_glyphs' wrapped-text policy).
// Used by the loading interstitial so the footer riffles in the board's own cells.
int hero_station_glyphs(GRect bounds, const char *station, HeroGlyph *out, int max);

// The station-footer band (full width) in canvas coords, for overlay effects
// such as the arrival wipe. Height is clamped to the bounds.
GRect hero_station_rect(GRect bounds);

// The bounding rect of the footer clock as hero_draw_footer paints it: its own
// line below a one-line station name, or the second wrapped line (where the inline
// "· HH:MM" sits) for a two-line name. For the arrival gold wipe to sweep the clock
// along with the rest of the board.
GRect hero_clock_rect(GRect bounds, const char *station);

// Bounce the bullet-disc + countdown band vertically by `dy` pixels (a framebuffer
// post-pass over the just-drawn board), leaving the rest of the hero put. Used for
// the single-line "nothing here" nudge. The vacated edge fills with background and
// the far edge clips — `dy` should stay small. Color only (no-op on b/w).
void hero_bounce_band(GContext *ctx, GRect bounds, int dy);

