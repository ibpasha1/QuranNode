// scene_reader.c — the focus reader (synced recitation).
//
// Current ayah large and centered, previous/next dimmed above/below. Recitation
// plays through the Player; the word the reciter is on is highlighted live
// (Player.active_word -> the pre-baked word box). The encoder is the primary
// control: while playing it changes speed (pitch-preserved), while paused it
// moves between ayat. Press plays/pauses. This is "the product is the focus".
#include "scene.h"
#include "arabic_text.h"
#include "player.h"
#include "progress.h"
#include "prefs.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "plat.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static GlyphPack s_pack;
static bool   s_pack_ok = false;
static int    s_pack_size = -1;  // font size the loaded pack was built for
static int    s_bm_toast = 0;    // frames remaining on the "Bookmarked" confirmation
#define s_player g_player   // the reader drives the shared transport

// Height of the opaque transport panel (above the 11px hint rail).
#define FOOT_H 28

static void mmss(uint32_t ms, char *b, int n)
{
    uint32_t s = ms / 1000;
    snprintf(b, n, "%u:%02u", s / 60, s % 60);
}

// Load (or reload) the glyph pack for the current font-size preference.
static void ensure_pack(void)
{
    if (s_pack_ok && s_pack_size == g_prefs.font_size) return;
    if (s_pack_ok) { glyphpack_close(&s_pack); s_pack_ok = false; }
    s_pack_ok = glyphpack_open(&s_pack, prefs_font_pack());
    s_pack_size = g_prefs.font_size;
}

static const char *surah_name(int s)
{
    switch (s) {
    case 1:  return "Al-Fatihah";
    case 18: return "Al-Kahf";
    case 67: return "Al-Mulk";
    default: return "Surah";
    }
}

static const char *reciter_name(const char *id)
{
    if (id && strcmp(id, "abdulbasit") == 0) return "Abdul Basit";
    return id ? id : "";
}

static void on_enter(void)
{
    ensure_pack();
    // Ensure a clip is loaded, but don't disturb a loop the loop editor started.
    if (!s_player.clip)
        player_load(&s_player, s_player.surah, s_player.ayah);
}

static void save_resume(void)
{
    progress_set_resume(s_player.surah, s_player.ayah, s_player.rate);
}

static void on_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    player_update(&s_player);

    // Persist the resume point whenever the position changes (cheap; ayat are
    // seconds long). This is what makes one-press resume land exactly here.
    static int last_s = -1, last_a = -1;
    if (s_player.surah != last_s || s_player.ayah != last_a) {
        last_s = s_player.surah; last_a = s_player.ayah;
        save_resume();
    }
    if (s_bm_toast > 0) s_bm_toast--;
}

// NB: not named on_exit() — that collides with newlib's stdlib on_exit().
static void on_leave(void)
{
    save_resume();
}

// Tajweed palette — index matches tools/shape_quran.py RULE_COLOR / PREVIEW_PALETTE.
//   1 red=necessary madd · 2 amber=madd · 3 green=nasal · 4 blue=qalqala · 5 grey=silent
static const color_t TAJWEED_PAL[] = {
    THEME_TEXT,               // 0 default
    RGB565(255,  90,  90),    // 1 red
    RGB565(255, 170,  70),    // 2 amber
    RGB565( 70, 210, 130),    // 3 green
    RGB565( 95, 170, 255),    // 4 blue
    RGB565(120, 120, 135),    // 5 grey
};
#define TAJWEED_N ((int)(sizeof(TAJWEED_PAL) / sizeof(TAJWEED_PAL[0])))

// Draw the ayah at pack (surah:ayah), horizontally centered with its top at
// `top`. `colored` uses the tajweed palette (only for the focused ayah).
static int draw_ayah(Canvas *c, int surah, int ayah, int top, color_t col, int hl, bool colored)
{
    AyahGlyphs g;
    if (!glyphpack_get(&s_pack, surah, ayah, &g)) return 0;
    int x = (CANVAS_WIDTH - g.w) / 2;
    if (colored && g.colidx)
        arabic_draw_ayah_colored(c, x, top, &g, TAJWEED_PAL, TAJWEED_N, col, hl, THEME_PLAYHEAD);
    else
        arabic_draw_ayah(c, x, top, &g, col, hl, THEME_PLAYHEAD);
    return g.h;
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    ensure_pack();

    if (!s_pack_ok) {
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 - 4, &font_small,
                                  "No glyph pack. Run tools/shape_quran.py", THEME_DIM);
        theme_hint(c, "M0: build assets/packs/reader_lg.qgp");
        return;
    }

    int surah = s_player.surah, ayah = s_player.ayah;

    // Header: surah name + reference (a leading * marks a bookmarked ayah).
    char ref[20];
    bool marked = progress_is_bookmarked(surah, ayah);
    snprintf(ref, sizeof(ref), "%s%d:%d", marked ? "* " : "", surah, ayah);
    theme_header(c, surah_name(surah), THEME_TITLE, ref,
                 marked ? THEME_BADGE : THEME_LABEL);

    // Current ayah, centered in the main band; prev/next dimmed around it.
    AyahGlyphs cur;
    if (!glyphpack_get(&s_pack, surah, ayah, &cur)) {
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 - 12, &font_small,
                                  "Not in the bundled sample yet", THEME_DIM);
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 + 6, &font_tiny,
                                  "(only Al-Fatihah is loaded)", THEME_DIM);
        theme_hint(c, "<> ayah    BACK home");
        return;
    }
    // The transport panel is opaque and drawn AFTER the text, so no ayah can
    // ever collide with the position bar / reciter line again — overflow is
    // cleanly clipped behind the panel instead.
    int foot_y = CANVAS_HEIGHT - 11 - FOOT_H;   // panel top, above the hint rail
    {
        // Center the focused ayah in the reading band (below header, above the
        // transport panel); clamp the top so tall (large-font / multi-line)
        // ayat don't ride up under the header.
        int band_top = 26, band_bot = foot_y - 4;
        int cur_top = band_top + (band_bot - band_top - cur.h) / 2;
        if (cur_top < band_top) cur_top = band_top;
        // Focused ayah: tajweed-colored when enabled; prev/next stay dimmed mono.
        draw_ayah(c, surah, ayah, cur_top, THEME_TEXT, s_player.active_word, g_prefs.tajweed);

        // At large font sizes there's no room for context — show only the focus ayah.
        if (!prefs_font_is_large()) {
            AyahGlyphs prev;
            // Only draw the previous ayah if it fits above without touching the
            // header (it renders before us, so it would draw on top of it).
            if (glyphpack_get(&s_pack, surah, ayah - 1, &prev) &&
                cur_top - prev.h - 14 >= band_top)
                draw_ayah(c, surah, ayah - 1, cur_top - prev.h - 14, THEME_DIM, -1, false);
            // The next ayah may run long; the opaque panel below clips it.
            if (cur_top + cur.h + 14 < band_bot)
                draw_ayah(c, surah, ayah + 1, cur_top + cur.h + 14, THEME_DIM, -1, false);
        }
    }

    // Transport panel: position bar with elapsed/total, reciter + state + speed.
    canvas_rect_fill(c, 0, foot_y, CANVAS_WIDTH, CANVAS_HEIGHT - foot_y, THEME_PANEL);
    canvas_hline(c, 0, foot_y, CANVAS_WIDTH, THEME_GRID);

    uint32_t pos = player_pos_ms(&s_player), len = player_len_ms(&s_player);
    float frac = len ? (float)pos / (float)len : 0.f;
    canvas_progress_bar(c, 12, foot_y + 7, CANVAS_WIDTH - 24, 4, frac,
                        THEME_ACCENT, THEME_GRID);

    char tb[12];
    mmss(pos, tb, sizeof(tb));
    font_draw_string(c, 12, foot_y + 17, &font_tiny, tb, THEME_DIM);
    mmss(len, tb, sizeof(tb));
    font_draw_string_right(c, CANVAS_WIDTH - 12, foot_y + 17, &font_tiny, tb, THEME_DIM);

    char foot[64];
    if (s_player.loop.active) {
        // Loop mode: show range + repeat progress instead of the reciter line.
        char sec[12];
        if (s_player.loop.section_reps == 0)
            snprintf(sec, sizeof(sec), "%d/inf", s_player.loop_section_i + 1);
        else
            snprintf(sec, sizeof(sec), "%d/%d", s_player.loop_section_i + 1,
                     s_player.loop.section_reps);
        snprintf(foot, sizeof(foot), "LOOP %d:%d-%d x%d sec %s %.2fx",
                 s_player.surah, s_player.loop.start_ayah, s_player.loop.end_ayah,
                 s_player.loop.each_reps, sec, s_player.rate);
        font_draw_string_centered(c, foot_y + 17, &font_tiny, foot, THEME_BADGE);
    } else {
        snprintf(foot, sizeof(foot), "%s %s  %.2fx",
                 s_player.playing ? ">" : "||",
                 reciter_name(s_player.reciter),
                 s_player.rate);
        font_draw_string_centered(c, foot_y + 17, &font_tiny, foot,
                                  s_player.playing ? THEME_TEXT : THEME_LABEL);
    }

    // "Bookmarked" toast: small chip floating above the transport panel.
    if (s_bm_toast > 0) {
        int tw = font_string_width(&font_tiny, "Bookmarked") + 16;
        int tx = (CANVAS_WIDTH - tw) / 2, ty = foot_y - 20;
        canvas_rect_fill(c, tx, ty, tw, 14, THEME_PANEL);
        canvas_rect(c, tx, ty, tw, 14, THEME_ACTIVE);
        font_draw_string_centered(c, ty + 4, &font_tiny, "Bookmarked", THEME_ACTIVE);
    }

    theme_hint(c, s_player.loop.active ? "MODE edit loop   SEL pause   <>exit loop   BACK home"
             : s_player.playing ? "ENC speed   SEL pause   MODE loop   BACK home"
                                : "ENC ayah  K bookmark  MODE loop  BACK home");
}

static void on_input(InputEvent e)
{
    switch (e.type) {
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:
    case INPUT_BTN_PLAY:
        player_toggle(&s_player);
        break;

    // Encoder: speed while playing, ayah navigation while paused.
    case INPUT_ENC_CW:
        if (s_player.playing) player_nudge_rate(&s_player, +1);
        else player_next_ayah(&s_player);
        break;
    case INPUT_ENC_CCW:
        if (s_player.playing) player_nudge_rate(&s_player, -1);
        else player_prev_ayah(&s_player);
        break;

    // D-pad always moves between ayat.
    case INPUT_NAV_RIGHT:
    case INPUT_NAV_DOWN:
        player_next_ayah(&s_player);
        break;
    case INPUT_NAV_LEFT:
    case INPUT_NAV_UP:
        player_prev_ayah(&s_player);
        break;

    case INPUT_BTN_MODE:
        scene_switch(SCENE_LOOP);
        break;

    case INPUT_BTN_BOOKMARK:
        progress_add_bookmark(s_player.surah, s_player.ayah);
        s_bm_toast = 45;   // ~1.5s confirmation
        break;

    case INPUT_BTN_BACK:
    case INPUT_BTN_MENU:
        player_pause(&s_player);
        scene_switch(SCENE_HOME);
        break;
    default: break;
    }
}

static const SceneCallbacks CB = {
    .on_enter = on_enter,
    .on_exit = on_leave,
    .on_render = on_render,
    .on_input = on_input,
    .on_tick = on_tick,
};

void scene_reader_register(void) { scene_register(SCENE_READER, &CB); }
