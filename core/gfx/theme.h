#pragma once
#include "canvas.h"
#include "font.h"
#include "input.h"

// ---------------------------------------------------------------------------
// Retro-M8 visual theme (shared across scenes).
//
// This is purely the LOOK — the interaction model stays pad + function-key +
// encoder (an MPC/Push-class surface), NOT the M8's modal tracker nav. The
// theme gives every scene the same retro identity: near-black background, one
// small monospace font, flat blocks (no rounded corners), fixed header + hint
// rails, and a SOLID inverted selection block. Per-track colors are kept and
// are meant to be mirrored on the 16 per-pad RGB LEDs (screen + hands unified).
// ---------------------------------------------------------------------------

// --- Palette (QuranNode "mushaf" scheme: emerald + gold + ivory) -----------
// Color encodes ROLE, not decoration:
//   sage labels/indices · warm ivory values · gold cursor · green = playing/on ·
//   coral = badges · near-black emerald bg. Inspired by illuminated mushaf
//   margins — replaces the DSP-Mini cyan/white terminal look.
#define THEME_BG        RGB565(7, 10, 8)        // near-black, green cast
#define THEME_PANEL     RGB565(14, 21, 16)      // title/hint rail + card fill
#define THEME_ROW       RGB565(23, 34, 26)      // focused row wash (subtle)
#define THEME_GRID      RGB565(40, 56, 44)      // rails, separators, cell borders
#define THEME_DIM       RGB565(98, 110, 96)     // empty / inactive / "·"
#define THEME_TEXT      RGB565(232, 224, 200)   // values / data (warm ivory)
#define THEME_VALUE     THEME_TEXT              // alias: edited value text
#define THEME_TITLE     RGB565(240, 198, 100)   // screen name (soft gold)
#define THEME_LABEL     RGB565(148, 170, 136)   // labels / row indices / tabs (sage)
#define THEME_ACCENT    RGB565(255, 200, 80)    // cursor / selection / focus (GOLD)
#define THEME_ACTIVE    RGB565(90, 220, 130)    // playing / on / confirm (green)
#define THEME_BADGE     RGB565(255, 140, 60)    // badges: xN repeat, LOOP (coral)
#define THEME_PLAYHEAD  RGB565(46, 38, 14)      // active-word wash (dark gold)

// Solid cursor block + contrasting text — the signature highlight (now gold).
#define THEME_SEL_BG    RGB565(255, 200, 80)    // gold cursor block
#define THEME_SEL_TEXT  RGB565(10, 12, 8)       // dark text on the block

// Secondary fills.
#define THEME_CLIP      RGB565(230, 40, 150)     // copy/clipboard row outline (magenta)
#define THEME_BAR       RGB565(176, 138, 58)     // value-bar fill (muted gold)

// The 8 per-track hues (also driven onto the pad RGB LEDs).
color_t theme_track_color(int t);

// File-system / menu icons — small ~9px vector glyphs drawn at (x,y). Shared by
// the tree menu and the list-menu component so every page reads the same.
typedef enum {
    ICON_NONE = 0, ICON_FOLDER, ICON_GEAR, ICON_NOTE, ICON_WAVE,
    ICON_SPEAKER, ICON_CLOUD, ICON_FX, ICON_SLIDER, ICON_TOGGLE, ICON_INFO,
    ICON_BOOK, ICON_REPEAT,
} UiIcon;
void theme_icon(Canvas *c, int x, int y, UiIcon icon, color_t col, color_t bg);

// --- Primitives ------------------------------------------------------------
void theme_clear(Canvas *c);

// Top rail: title (left) + optional right-status, with a separator beneath.
// Returns the y of the first content row below the rail.
int  theme_header(Canvas *c, const char *title, color_t title_col,
                  const char *right, color_t right_col);

// Bottom hint rail: subtle panel + dim monospace text.
void theme_hint(Canvas *c, const char *hint);

// --- Interactive key guide (bottom rail) -----------------------------------
// A row of chips, one per physical control: [KEY] ACTION. Scenes pass the
// chips that apply RIGHT NOW, so the rail is a live map of the buttons — and
// the chip whose button was just pressed flashes gold, closing the loop
// between hands and screen. app_input() feeds every event to
// theme_keybar_note() to drive the flash.
#define THEME_KEYBAR_H 16
typedef struct {
    const char    *key;      // physical-key glyph: "^v" "<>" "OK" "BK" "MD"...
    const char    *action;   // what it does in this state: "OPEN" "PAUSE"...
    uint8_t        ev_n;     // events this chip covers (flash matching)
    InputEventType evs[4];
} KeyChip;

void theme_keybar(Canvas *c, const KeyChip *chips, int n);
void theme_keybar_note(InputEventType t);

// Solid, hard-edged selection block.
void theme_sel_block(Canvas *c, int x, int y, int w, int h);

// Monospace cell text centered on cx within a `cellw`-wide slot. When `sel`,
// paints the magenta cursor block and overrides the color to THEME_SEL_TEXT.
void theme_cell(Canvas *c, int cx, int y, int cellw, const char *s, bool sel, color_t col);

// M8-style mini-map: a small strip of `n` cells at (x,y); `cur` lit in accent,
// the rest dim — a compact "where am I" position indicator (bottom/top-right).
void theme_minimap(Canvas *c, int x, int y, int n, int cur);

// --- Shared components (the UI vocabulary; see Docs/UI_DIRECTION.md) --------
// Cyan cursor outline for a grid cell (non-filled — use for grids/matrices).
void theme_cursor_box(Canvas *c, int x, int y, int w, int h);

// A full-width list row: LABEL (left, soft-blue) + VALUE (right, white),
// `font_small`. `sel` paints the cyan block + dark text; `dim` greys the value.
void theme_row(Canvas *c, int y, const char *label, const char *value,
               bool sel, bool dim);

// A value bar filled to `norm` (0..1) in cyan, on a grid-colored track.
void theme_bar(Canvas *c, int x, int y, int w, int h, float norm, bool sel);

// Live oscilloscope of `buf[0..len)` (samples in [-1,1]); auto-gains to fill.
// Draws the framed box; cyan trace when there's signal, else a flat baseline.
void theme_scope(Canvas *c, int x, int y, int w, int h, const float *buf, int len);

// Segmented VU meter, `level` 0..1, green→yellow→red.
void theme_meter(Canvas *c, int x, int y, int w, int h, float level);

// --- v2 M8-view chrome (see Docs/V2_UI_SPEC.md) ----------------------------
// Big top-left screen title (font_medium). Returns the y of the first content
// row below it. `col` = THEME_ACTIVE when the view is "playing", else THEME_TITLE.
int  theme_title_big(Canvas *c, const char *title, color_t col);

// The persistent right rail (VU + tempo + 1..8 track column + mini piano).
// Returns the rail's LEFT x — content must stay left of it. `track_notes` is 8
// MIDI notes (<0 = empty/off); `cur_track` is drawn in its track color. `held12`
// is a 12-bit pitch-class mask lighting the mini piano (0 = none).
int  theme_rail(Canvas *c, int tempo, bool playing, int cur_track,
                const int *track_notes, float vu, uint16_t held12);
#define THEME_RAIL_W 40
