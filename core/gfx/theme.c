#include "theme.h"
#include "plat.h"
#include <stdio.h>

// Per-track identity hues. Kept vivid on purpose — these double as the pad RGB
// LED colors, tying the on-screen tracks to the physical pads.
static const color_t s_track[8] = {
    COLOR_CYAN, COLOR_ORANGE, COLOR_GREEN, COLOR_MAGENTA,
    COLOR_YELLOW, COLOR_BLUE, COLOR_RED, COLOR_TEAL,
};

color_t theme_track_color(int t) { return s_track[t & 7]; }

void theme_icon(Canvas *c, int x, int y, UiIcon icon, color_t col, color_t bg)
{
    switch (icon) {
    case ICON_FOLDER:
        canvas_rect_fill(c, x, y + 1, 5, 2, col);
        canvas_rect_fill(c, x, y + 2, 10, 6, col);
        canvas_hline(c, x + 1, y + 3, 8, bg);
        break;
    case ICON_GEAR:
        canvas_circle_fill(c, x + 4, y + 4, 4, col);
        canvas_circle_fill(c, x + 4, y + 4, 1, bg);
        canvas_rect_fill(c, x + 3, y - 1, 2, 2, col); canvas_rect_fill(c, x + 3, y + 7, 2, 2, col);
        canvas_rect_fill(c, x - 1, y + 3, 2, 2, col); canvas_rect_fill(c, x + 7, y + 3, 2, 2, col);
        break;
    case ICON_NOTE:
        canvas_circle_fill(c, x + 2, y + 7, 2, col);
        canvas_vline(c, x + 4, y, 7, col); canvas_line(c, x + 4, y, x + 8, y + 2, col);
        break;
    case ICON_WAVE:
        canvas_line(c, x, y + 7, x + 2, y + 1, col); canvas_line(c, x + 2, y + 1, x + 4, y + 7, col);
        canvas_line(c, x + 4, y + 7, x + 6, y + 1, col); canvas_line(c, x + 6, y + 1, x + 8, y + 7, col);
        break;
    case ICON_SPEAKER:
        canvas_rect_fill(c, x, y + 3, 3, 3, col);
        canvas_line(c, x + 3, y + 4, x + 5, y + 1, col); canvas_line(c, x + 3, y + 4, x + 5, y + 8, col);
        canvas_vline(c, x + 5, y + 1, 8, col);
        canvas_line(c, x + 7, y + 2, x + 8, y + 4, col); canvas_line(c, x + 7, y + 7, x + 8, y + 5, col);
        break;
    case ICON_CLOUD:
        canvas_circle_fill(c, x + 3, y + 5, 3, col); canvas_circle_fill(c, x + 6, y + 4, 3, col);
        canvas_rect_fill(c, x + 2, y + 5, 7, 3, col);
        break;
    case ICON_FX:
        canvas_vline(c, x + 4, y, 9, col); canvas_hline(c, x, y + 4, 9, col);
        canvas_line(c, x + 1, y + 1, x + 7, y + 7, col); canvas_line(c, x + 7, y + 1, x + 1, y + 7, col);
        break;
    case ICON_SLIDER:
        canvas_hline(c, x, y + 4, 10, col);                // track
        canvas_rect_fill(c, x + 5, y + 2, 3, 5, col);      // knob
        break;
    case ICON_TOGGLE:
        canvas_rect(c, x, y + 2, 10, 5, col);              // switch body
        canvas_rect_fill(c, x + 5, y + 2, 4, 5, col);      // knob (on the right)
        break;
    case ICON_INFO:
        canvas_circle_fill(c, x + 4, y + 1, 1, col);       // dot
        canvas_vline(c, x + 4, y + 3, 5, col);             // stem
        break;
    case ICON_BOOK:                                        // open mushaf
        canvas_rect_fill(c, x, y + 1, 4, 7, col);
        canvas_rect_fill(c, x + 5, y + 1, 4, 7, col);
        canvas_vline(c, x + 4, y, 9, col);                 // spine
        canvas_hline(c, x, y + 8, 9, col);
        break;
    case ICON_REPEAT:                                      // loop arrows
        canvas_circle(c, x + 4, y + 4, 3, col);
        canvas_rect_fill(c, x + 6, y, 3, 3, col);          // arrowhead
        break;
    default: break;
    }
}

void theme_clear(Canvas *c) { canvas_clear(c, THEME_BG); }

int theme_header(Canvas *c, const char *title, color_t title_col,
                 const char *right, color_t right_col)
{
    canvas_rect_fill(c, 0, 0, CANVAS_WIDTH, 13, THEME_PANEL);
    font_draw_string(c, 6, 3, &font_tiny, title, title_col);
    if (right)
        font_draw_string_right(c, CANVAS_WIDTH - 6, 3, &font_tiny, right, right_col);
    canvas_hline(c, 0, 13, CANVAS_WIDTH, THEME_GRID);
    return 17;
}

void theme_hint(Canvas *c, const char *hint)
{
    int y = CANVAS_HEIGHT - 11;
    canvas_rect_fill(c, 0, y, CANVAS_WIDTH, 11, THEME_PANEL);
    canvas_hline(c, 0, y, CANVAS_WIDTH, THEME_GRID);
    font_draw_string(c, 6, y + 2, &font_tiny, hint, THEME_DIM);
}

void theme_sel_block(Canvas *c, int x, int y, int w, int h)
{
    canvas_rect_fill(c, x, y, w, h, THEME_SEL_BG);
}

// --- Interactive key guide --------------------------------------------------
static InputEventType s_kb_last = INPUT_NONE;
static uint32_t       s_kb_ms;

void theme_keybar_note(InputEventType t)
{
    s_kb_last = t;
    s_kb_ms = plat_millis();
}

void theme_keybar(Canvas *c, const KeyChip *chips, int n)
{
    int y = CANVAS_HEIGHT - THEME_KEYBAR_H;
    canvas_rect_fill(c, 0, y, CANVAS_WIDTH, THEME_KEYBAR_H, THEME_PANEL);
    canvas_hline(c, 0, y, CANVAS_WIDTH, THEME_GRID);

    // The chip whose button fired in the last few frames lights up gold.
    bool fresh = (uint32_t)(plat_millis() - s_kb_ms) < 160;

    int x = 6;
    for (int i = 0; i < n; i++) {
        const KeyChip *k = &chips[i];
        bool hot = false;
        if (fresh)
            for (int e = 0; e < k->ev_n; e++)
                if (k->evs[e] == s_kb_last) { hot = true; break; }

        int kw = font_string_width(&font_tiny, k->key) + 6;
        canvas_rect_fill(c, x, y + 3, kw, 11, hot ? THEME_ACCENT : THEME_GRID);
        font_draw_string(c, x + 3, y + 5, &font_tiny, k->key,
                         hot ? THEME_SEL_TEXT : THEME_TEXT);
        x += kw + 4;
        font_draw_string(c, x, y + 5, &font_tiny, k->action,
                         hot ? THEME_ACCENT : THEME_DIM);
        x += font_string_width(&font_tiny, k->action) + 10;
    }
}

void theme_cell(Canvas *c, int cx, int y, int cellw, const char *s, bool sel, color_t col)
{
    if (sel) {
        theme_sel_block(c, cx - cellw / 2, y - 2, cellw, 11);
        col = THEME_SEL_TEXT;
    }
    int tw = font_string_width(&font_tiny, s);
    font_draw_string(c, cx - tw / 2, y, &font_tiny, s, col);
}

void theme_minimap(Canvas *c, int x, int y, int n, int cur)
{
    const int cw = 5, ch = 5, gap = 2;
    for (int i = 0; i < n; i++) {
        int cx = x + i * (cw + gap);
        canvas_rect_fill(c, cx, y, cw, ch, i == cur ? THEME_ACCENT : THEME_GRID);
    }
}

// --- Shared components -----------------------------------------------------
void theme_cursor_box(Canvas *c, int x, int y, int w, int h)
{
    canvas_rect(c, x, y, w, h, THEME_ACCENT);
}

void theme_row(Canvas *c, int y, const char *label, const char *value,
               bool sel, bool dim)
{
    if (sel) theme_sel_block(c, 0, y - 2, CANVAS_WIDTH, 16);
    color_t lc = sel ? THEME_SEL_TEXT : THEME_LABEL;
    color_t vc = sel ? THEME_SEL_TEXT : (dim ? THEME_DIM : THEME_TEXT);
    font_draw_string(c, 10, y, &font_small, label, lc);
    font_draw_string_right(c, CANVAS_WIDTH - 10, y + 1, &font_small, value, vc);
}

void theme_bar(Canvas *c, int x, int y, int w, int h, float norm, bool sel)
{
    if (norm < 0.0f) norm = 0.0f; else if (norm > 1.0f) norm = 1.0f;
    canvas_rect_fill(c, x, y, w, h, sel ? THEME_ROW : THEME_GRID);
    canvas_rect_fill(c, x, y, (int)(w * norm), h, sel ? THEME_SEL_TEXT : THEME_ACCENT);
}

void theme_scope(Canvas *c, int x, int y, int w, int h, const float *buf, int len)
{
    canvas_rect(c, x, y, w, h, THEME_GRID);
    int x0 = x + 1, w0 = w - 2, midy = y + h / 2, amp = h / 2 - 2, prev = midy;
    float peak = 0.0f;
    for (int i = 0; i < len; i++) {
        float a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    if (peak <= 0.02f) { canvas_hline(c, x0, midy, w0, THEME_DIM); return; }   // silent
    float gain = 0.9f / peak;
    for (int px = 0; px < w0; px++) {
        float s = buf[(px * len) / w0] * gain;
        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
        int py = midy - (int)(s * amp);
        int a = py < prev ? py : prev, b = py < prev ? prev : py;
        canvas_rect_fill(c, x0 + px, a, 1, b - a + 1, THEME_ACCENT);
        prev = py;
    }
}

void theme_meter(Canvas *c, int x, int y, int w, int h, float level)
{
    if (level < 0.0f) level = 0.0f; else if (level > 1.0f) level = 1.0f;
    canvas_rect_fill(c, x, y, w, h, THEME_PANEL);
    int fw = (int)(w * level);
    for (int i = 0; i < fw; i++) {
        float f = (float)i / (float)w;
        color_t col = f < 0.6f ? THEME_ACTIVE : (f < 0.8f ? COLOR_YELLOW : COLOR_RED);
        canvas_vline(c, x + i, y, h, col);
    }
    canvas_rect(c, x, y, w, h, THEME_GRID);
}

// --- v2 M8-view chrome -----------------------------------------------------
int theme_title_big(Canvas *c, const char *title, color_t col)
{
    font_draw_string(c, 6, 4, &font_medium, title, col);
    return 26;
}

// MIDI note -> "C-4" / "C#1" style (1-char names get a '-' filler; MIDI 60=C4).
static void theme_note_name(int n, char *b, int sz)
{
    static const char *NN[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    if (n < 0) { snprintf(b, sz, "---"); return; }
    const char *nm = NN[n % 12];
    int oct = n / 12 - 1;
    if (nm[1] == '\0') snprintf(b, sz, "%c-%d", nm[0], oct);
    else               snprintf(b, sz, "%s%d", nm, oct);
}

// Compact one-octave piano; lights the held pitch classes (12-bit mask).
static void theme_mini_piano(Canvas *c, int x, int y, int w, int h, uint16_t held)
{
    static const int white[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static const int black[5] = { 1, 3, 6, 8, 10 };
    static const int bpos[5]  = { 0, 1, 3, 4, 5 };   // white index each black sits after
    int kw = w / 7;
    canvas_rect_fill(c, x, y, kw * 7 + 1, h, THEME_PANEL);
    for (int i = 0; i < 7; i++) {
        int kx = x + i * kw;
        bool on = (held >> white[i]) & 1;
        canvas_rect_fill(c, kx + 1, y + 1, kw - 1, h - 2, on ? THEME_ACCENT : THEME_PANEL);
        canvas_vline(c, kx, y, h, THEME_GRID);
    }
    canvas_vline(c, x + kw * 7, y, h, THEME_GRID);
    for (int i = 0; i < 5; i++) {
        int kx = x + bpos[i] * kw + kw - kw / 3;
        bool on = (held >> black[i]) & 1;
        canvas_rect_fill(c, kx, y, (kw * 2) / 3, h / 2 + 1, on ? THEME_ACCENT : THEME_BG);
    }
    canvas_rect(c, x, y, kw * 7 + 1, h, THEME_GRID);
}

int theme_rail(Canvas *c, int tempo, bool playing, int cur_track,
               const int *track_notes, float vu, uint16_t held12)
{
    const int rx = CANVAS_WIDTH - THEME_RAIL_W;
    canvas_vline(c, rx, 0, CANVAS_HEIGHT, THEME_GRID);

    // VU meter (top).
    theme_meter(c, rx + 3, 4, THEME_RAIL_W - 7, 6, vu);

    // Tempo readout.
    char tb[10];
    snprintf(tb, sizeof(tb), "T\x10%d", tempo & 0x1FF);
    font_draw_string(c, rx + 3, 14, &font_tiny, tb, playing ? THEME_ACTIVE : THEME_LABEL);

    // Track column 1..8 (index + optional note readout).
    int ty = 26;
    for (int t = 0; t < 8; t++, ty += 11) {
        char nb[8];
        snprintf(nb, sizeof(nb), "%d", t + 1);
        bool cur = (t == cur_track);
        font_draw_string(c, rx + 3, ty, &font_tiny, nb, cur ? theme_track_color(t) : THEME_DIM);
        char nn[6];
        theme_note_name(track_notes ? track_notes[t] : -1, nn, sizeof(nn));
        font_draw_string(c, rx + 11, ty, &font_tiny, nn, cur ? THEME_TEXT : THEME_DIM);
    }

    // Mini piano (below the track column).
    theme_mini_piano(c, rx + 3, ty + 4, THEME_RAIL_W - 8, 12, held12);
    return rx;
}
