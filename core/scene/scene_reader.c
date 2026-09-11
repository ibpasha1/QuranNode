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
#include "khatm.h"
#include "prefs.h"
#include "quran_db.h"
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
static int    s_pack_tj = -1;    // tajweed flag the loaded pack matches
static int    s_pack_surah = -1; // surah the loaded pack is for (packs are per-surah)
static int    s_bm_toast = 0;    // frames remaining on the "Bookmarked" confirmation
#define s_player g_player   // the reader drives the shared transport

// Vertical scroll for ayat taller than the screen (~43% of ayat at Large).
// While playing, the view follows the recited word; while paused, Up/Down drive
// s_manual_scroll. s_scroll is the eased, currently-applied offset (px from the
// ayah's top); s_scroll_key detects an ayah change so the scroll resets.
static float  s_scroll = 0.f;
static int    s_manual_scroll = 0;
static bool   s_was_playing = false; // prev frame's play state, to catch pause
static int    s_scroll_key = -1;
static bool   s_overflow = false;   // set by on_render, read by on_input
static int    s_max_scroll = 0;     // set by on_render, read by on_input
#define SCROLL_STEP 60              // px per Up/Down press while paused

// Height of the opaque transport panel (above the keybar).
#define FOOT_H 28

static void mmss(uint32_t ms, char *b, int n)
{
    uint32_t s = ms / 1000;
    snprintf(b, n, "%u:%02u", s / 60, s % 60);
}

// Load (or reload) the glyph pack for the current font-size + tajweed prefs.
// Keyed on BOTH: the colored packs are separate _tj files, so flipping the
// tajweed toggle must reload just like a size change (this was the "tajweed
// colors don't work" bug — the mono pack stayed loaded until a size change).
static void ensure_pack(void)
{
    if (s_pack_ok && s_pack_size == g_prefs.font_size &&
        s_pack_tj == g_prefs.tajweed && s_pack_surah == s_player.surah)
        return;
    if (s_pack_ok) { glyphpack_close(&s_pack); s_pack_ok = false; }
    s_pack_ok = glyphpack_open(&s_pack, prefs_font_pack(s_player.surah));
    s_pack_size = g_prefs.font_size;
    s_pack_tj = g_prefs.tajweed;
    s_pack_surah = s_player.surah;
}

static const char *surah_name(int s)
{
    const char *n = qdb_surah_name(s);
    return (n && n[0]) ? n : "Surah";
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
    player_update(&s_player);

    // Reading credit. An ayah counts once it has been on screen long enough to
    // plausibly have been read, or once its recitation played through — the
    // player raises done_seq for the latter. khatm_focus() is fed the word
    // count in on_render, where the glyph pack is available.
    static uint32_t last_done = 0;
    if (s_player.done_seq != last_done) {
        last_done = s_player.done_seq;
        khatm_audio_complete(s_player.done_surah, s_player.done_ayah);
    }
    khatm_tick(dt_ms);

    // Persist the resume point once the position SETTLES. Saving on every
    // change looked cheap, but a held encoder ramps to a ~40ms repeat, which
    // meant ~25 SD writes a second while scrolling.
    static int last_s = -1, last_a = -1;
    static bool pending = false;
    if (s_player.surah != last_s || s_player.ayah != last_a) {
        last_s = s_player.surah; last_a = s_player.ayah;
        pending = true;
    }
    if (pending && khatm_focus_dwell_ms() >= 1500) {
        pending = false;
        save_resume();
    }
    if (s_bm_toast > 0) s_bm_toast--;
}

// NB: not named on_exit() — that collides with newlib's stdlib on_exit().
static void on_leave(void)
{
    save_resume();
    khatm_flush();
}

// Tajweed palette — index matches tools/shape_quran.py RULE_COLOR / PREVIEW_PALETTE.
//   1 red=obligatory madd · 2 amber=madd · 3 green=ghunnah/ikhfa · 4 blue=qalqala
//   5 grey=silent/merged · 6 dark blue=tafkhim (heavy raa + laam of Allah)
static const color_t TAJWEED_PAL[] = {
    THEME_TEXT,               // 0 default
    RGB565(255,  90,  90),    // 1 red
    RGB565(255, 170,  70),    // 2 amber
    RGB565( 70, 210, 130),    // 3 green
    RGB565( 95, 170, 255),    // 4 blue
    RGB565(120, 120, 135),    // 5 grey
    RGB565( 70, 120, 235),    // 6 dark blue (tafkhim)
};
#define TAJWEED_N ((int)(sizeof(TAJWEED_PAL) / sizeof(TAJWEED_PAL[0])))

// Ayah 1 of every surah but 1 (which *is* the basmala) and 9 (which has none)
// is drawn with a leading 4-word "Bismillah…" that the timing data doesn't
// carry — the recitation clips start at the ayah's own first word.
#define BASMALA_WORDS 4

// Map the reciter's timing word index onto the drawn glyph words. They line up
// 1:1 for most ayat, but the drawn ayah 1 has the basmala prefix the timing
// lacks (so the highlight would otherwise sit 4 words too early, on the
// basmala, while the audio recites the actual ayah), and ~12% of ayat split
// words differently. Returns -1 only when there's nothing to show yet.
static int hl_word(int surah, int ayah, int active_word)
{
    if (active_word < 0) return -1;
    int tw = timing_word_count(&s_player.timing, ayah);
    if (tw <= 0) return -1;
    if (qdb_words_agree(surah, ayah, tw)) return active_word;   // splits line up 1:1
    int dw = qdb_word_count(surah, ayah);
    if (dw <= 0) return -1;
    // Ayah 1 carries the basmala prefix the timing lacks: skip past it so the
    // remaining words line up 1:1 with the reciter.
    if (ayah == 1 && surah != 1 && surah != 9 && dw == tw + BASMALA_WORDS)
        return active_word + BASMALA_WORDS;
    // Splits disagree some other way (658 ayat, occasionally by many words).
    // Sweep the highlight proportionally so it still tracks the recitation
    // across the whole ayah — approximate at the word boundaries, but
    // continuous — instead of dropping it (which stalls the highlight and the
    // follow-scroll for the entire ayah).
    int mapped = (int)(((float)active_word + 0.5f) * (float)dw / (float)tw);
    if (mapped < 0) mapped = 0;
    if (mapped >= dw) mapped = dw - 1;
    return mapped;
}

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

// Reading-credit chrome, in the 9px gap between the header and the text band.
//
// Top: one segment per ayah on the current mushaf page, lit where you've read
// it — "how much of this page is done" at a glance. Below it: the dwell bar for
// the ayah you're on, filling until it counts. Showing the dwell is the point;
// without it, crediting looks arbitrary and people wonder why a page they
// flicked through didn't register.
static void draw_page_strip(Canvas *c, int page, int surah, int ayah)
{
    if (page < 1) return;
    const int x0 = 12, w = CANVAS_WIDTH - 24, y = 16;
    int n = qdb_page_ayah_count(page);
    if (n < 1) return;
    int first = qdb_page_first_global(page);
    int here = qdb_global_index(surah, ayah);

    for (int i = 0; i < n; i++) {
        int sx = x0 + (w * i) / n;
        int sw = x0 + (w * (i + 1)) / n - sx - 1;
        if (sw < 1) sw = 1;
        QRef r = qdb_from_global(first + i);
        color_t col = (first + i == here) ? THEME_ACCENT
                    : khatm_is_read(r.surah, r.ayah) ? THEME_ACTIVE
                    : THEME_GRID;
        canvas_rect_fill(c, sx, y, sw, 3, col);
    }

    if (!khatm_focus_credited()) {
        float f = khatm_focus_dwell_frac();
        canvas_rect_fill(c, x0, y + 5, w, 2, THEME_PANEL);
        if (f > 0.f) canvas_rect_fill(c, x0, y + 5, (int)(w * f), 2, THEME_BAR);
    }
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
    char ref[28];
    bool marked = progress_is_bookmarked(surah, ayah);
    int page = qdb_page_of(surah, ayah);
    snprintf(ref, sizeof(ref), "%s%d:%d  p%d", marked ? "* " : "", surah, ayah,
             page);
    theme_header(c, surah_name(surah), THEME_TITLE, ref,
                 marked ? THEME_BADGE : THEME_LABEL);

    draw_page_strip(c, page, surah, ayah);

    // Current ayah, centered in the main band; prev/next dimmed around it.
    AyahGlyphs cur;
    if (!glyphpack_get(&s_pack, surah, ayah, &cur)) {
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 - 12, &font_small,
                                  "This ayah isn't on the SD card", THEME_DIM);
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 + 6, &font_tiny,
                                  "Copy the repo's sdcard/ to the card", THEME_DIM);
        theme_hint(c, "<> ayah    BACK home");
        return;
    }
    // The word count sizes the dwell needed to credit this ayah as read. It is
    // only known once the pack has loaded, so it arrives here rather than in
    // on_tick (which runs first on the very frame an ayah changes).
    khatm_focus(surah, ayah, cur.n_words);
    // The transport panel is opaque and drawn AFTER the text, so no ayah can
    // ever collide with the position bar / reciter line again — overflow is
    // cleanly clipped behind the panel instead.
    int foot_y = CANVAS_HEIGHT - THEME_KEYBAR_H - FOOT_H;   // above the keybar
    {
        // Reading band: below the header, above the (opaque) transport panel.
        int band_top = 26, band_bot = foot_y - 4;
        int view_h = foot_y - band_top;          // full height the panel leaves us
        bool overflow = (cur.h > view_h);

        // Reset the scroll whenever the focused ayah changes.
        int ayah_key = (surah << 16) | (ayah & 0xffff);
        if (s_scroll_key != ayah_key) {
            s_scroll_key = ayah_key; s_scroll = 0.f; s_manual_scroll = 0;
        }
        s_overflow = overflow;
        s_max_scroll = overflow ? (cur.h - view_h) : 0;

        // On pause, freeze the scroll where the follow left it rather than
        // snapping to the (stale) manual baseline — otherwise the ayah jerks
        // up on pause and eases back down on resume. Seeding s_manual_scroll
        // from the current offset also lets Up/Down continue from here.
        if (s_was_playing && !s_player.playing) {
            int m = (int)(s_scroll + 0.5f);
            if (m < 0) m = 0;
            if (m > s_max_scroll) m = s_max_scroll;
            s_manual_scroll = m;
        }
        s_was_playing = s_player.playing;

        if (!overflow) {
            // Fits: center in the band, clamped so it can't ride under the header.
            int cur_top = band_top + (band_bot - band_top - cur.h) / 2;
            if (cur_top < band_top) cur_top = band_top;
            // Focused ayah: tajweed-colored when enabled.
            draw_ayah(c, surah, ayah, cur_top, THEME_TEXT,
                      hl_word(surah, ayah, s_player.active_word), g_prefs.tajweed);

            // At large font sizes there's no room for context — focus ayah only.
            if (!prefs_font_is_large()) {
                // Context ayat are also tajweed-colored when enabled (whole-page
                // Mushaf feel); THEME_DIM is the base tint, so their plain text
                // stays subdued while tajweed letters still show their rule colour.
                bool tj = g_prefs.tajweed;
                AyahGlyphs prev;
                // Only draw the previous ayah if it fits above without touching the
                // header (it renders before us, so it would draw on top of it).
                if (glyphpack_get(&s_pack, surah, ayah - 1, &prev) &&
                    cur_top - prev.h - 14 >= band_top)
                    draw_ayah(c, surah, ayah - 1, cur_top - prev.h - 14, THEME_DIM, -1, tj);
                // The next ayah may run long; the opaque panel below clips it.
                if (cur_top + cur.h + 14 < band_bot)
                    draw_ayah(c, surah, ayah + 1, cur_top + cur.h + 14, THEME_DIM, -1, tj);
            }
        } else {
            // Taller than the screen: scroll it. While playing, keep the recited
            // word ~42% down the view (karaoke follow); while paused, honour the
            // manual Up/Down scroll. No context ayat in this mode.
            int target;
            AtWordBox b;
            int hlw = hl_word(surah, ayah, s_player.active_word);
            if (s_player.playing) {
                if (hlw >= 0 && ayah_word_box(&cur, hlw, &b))
                    target = (b.y + b.h / 2) - (int)(view_h * 0.42f);
                else
                    target = (int)s_scroll;   // hold between words / at ayah end
            } else {
                target = s_manual_scroll;
            }
            if (target < 0) target = 0;
            if (target > s_max_scroll) target = s_max_scroll;
            // Ease toward the target so the follow-scroll glides, not jumps.
            float d = (float)target - s_scroll; if (d < 0) d = -d;
            if (d < 0.75f) s_scroll = (float)target;
            else           s_scroll += ((float)target - s_scroll) * 0.25f;

            int top = band_top - (int)(s_scroll + 0.5f);
            draw_ayah(c, surah, ayah, top, THEME_TEXT, hlw, g_prefs.tajweed);

            // The ayah scrolled up into the header band — repaint the header over it
            // (mirrors how the opaque transport panel masks the bottom overflow).
            canvas_rect_fill(c, 0, 0, CANVAS_WIDTH, band_top, THEME_BG);
            theme_header(c, surah_name(surah), THEME_TITLE, ref,
                         marked ? THEME_BADGE : THEME_LABEL);

            // Slim scrollbar on the right edge: how far through the ayah we are.
            int thumb_h = view_h * view_h / cur.h; if (thumb_h < 12) thumb_h = 12;
            int thumb_y = band_top + (int)((view_h - thumb_h) * (s_scroll / s_max_scroll));
            canvas_rect_fill(c, CANVAS_WIDTH - 3, band_top, 2, view_h, THEME_GRID);
            canvas_rect_fill(c, CANVAS_WIDTH - 3, thumb_y, 2, thumb_h, THEME_ACCENT);
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

    // Live keybar: the chips track the transport state, so the guide always
    // says what the buttons do RIGHT NOW (encoder = speed only while playing).
    if (s_player.loop.active) {
        KeyChip chips[4] = {
            { "OK", s_player.playing ? "PAUSE" : "PLAY", 3,
              { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "MD", "EDIT LOOP", 1, { INPUT_BTN_MODE } },
            { "<>", "EXIT", 4, { INPUT_NAV_LEFT, INPUT_NAV_RIGHT,
                                 INPUT_NAV_UP, INPUT_NAV_DOWN } },
            { "BK", "HOME", 2, { INPUT_BTN_BACK, INPUT_BTN_MENU } },
        };
        theme_keybar(c, chips, 4);
    } else if (s_player.playing) {
        KeyChip chips[4] = {
            { "OK", "PAUSE", 3,
              { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "<>", "SPEED", 2, { INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "MD", "LOOP", 1, { INPUT_BTN_MODE } },
            { "BK", "HOME", 2, { INPUT_BTN_BACK, INPUT_BTN_MENU } },
        };
        theme_keybar(c, chips, 4);
    } else {
        KeyChip chips[4] = {
            { "OK", "PLAY", 3,
              { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "<>", "AYAH", 4, { INPUT_ENC_CW, INPUT_ENC_CCW,
                                 INPUT_NAV_LEFT, INPUT_NAV_RIGHT } },
            { "KB", "MARK", 1, { INPUT_BTN_BOOKMARK } },
            { "BK", "HOME", 2, { INPUT_BTN_BACK, INPUT_BTN_MENU } },
        };
        theme_keybar(c, chips, 4);
    }
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

    // Left/Right always move between ayat. Up/Down scroll a too-tall ayah while
    // paused (see on_render); otherwise they also move between ayat.
    case INPUT_NAV_RIGHT:
        player_next_ayah(&s_player);
        break;
    case INPUT_NAV_LEFT:
        player_prev_ayah(&s_player);
        break;
    case INPUT_NAV_DOWN:
        if (!s_player.playing && s_overflow) {
            s_manual_scroll += SCROLL_STEP;
            if (s_manual_scroll > s_max_scroll) s_manual_scroll = s_max_scroll;
        } else player_next_ayah(&s_player);
        break;
    case INPUT_NAV_UP:
        if (!s_player.playing && s_overflow) {
            s_manual_scroll -= SCROLL_STEP;
            if (s_manual_scroll < 0) s_manual_scroll = 0;
        } else player_prev_ayah(&s_player);
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
