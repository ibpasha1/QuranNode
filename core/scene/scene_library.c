// scene_library.c — the "Library" personality: a plain local audio player.
//
// The device's second mode (separate from structured Quran): browse folders of
// nasheeds / lectures / audiobooks on the SD card and play them. Deliberately
// simple — a file browser plus a now-playing view — reusing the same audio HAL
// (dr_mp3 + SoundTouch) as the reader, so speed control works here too.
#include "scene.h"
#include "hal.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "input_accel.h"
#include "plat.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define LIB_ROOT "audio/library"
#define LIB_MAX  64

typedef enum { LIB_BROWSE, LIB_PLAYING } LibMode;

static LibMode s_mode = LIB_BROWSE;
static char s_dir[256];
static char s_folders[LIB_MAX][64]; static int s_nfolders;
static char s_files[LIB_MAX][64];   static int s_nfiles;
static int  s_sel, s_scroll;

// playback state
static struct HalAudioClip *s_clip;
static char  s_track[64];
static int   s_track_idx = -1;
static bool  s_playing;
static float s_rate = 1.0f;

#define ROW_H 20
#define LIST_TOP 28
#define VIS ((CANVAS_HEIGHT - 16 - LIST_TOP) / ROW_H)

static bool has_suffix(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcasecmp(s + ls - lf, suf) == 0;
}

static void strip_ext(const char *in, char *out, int n)
{
    snprintf(out, n, "%s", in);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static void load_dir(const char *dir)
{
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    s_nfolders = hal_fs_list(s_dir, s_folders, LIB_MAX, true);

    char all[LIB_MAX][64];
    int nall = hal_fs_list(s_dir, all, LIB_MAX, false);
    s_nfiles = 0;
    for (int i = 0; i < nall && s_nfiles < LIB_MAX; i++) {
        // keep audio files that aren't one of the subfolders
        bool is_dir = false;
        for (int f = 0; f < s_nfolders; f++)
            if (strcmp(all[i], s_folders[f]) == 0) { is_dir = true; break; }
        if (is_dir) continue;
        if (!has_suffix(all[i], ".mp3")) continue;
        snprintf(s_files[s_nfiles++], 64, "%s", all[i]);
    }
    s_sel = 0; s_scroll = 0;
}

static int entry_count(void) { return s_nfolders + s_nfiles; }

static void play_index(int file_i)
{
    if (file_i < 0 || file_i >= s_nfiles) return;
    char path[400];
    snprintf(path, sizeof(path), "%s/%s", s_dir, s_files[file_i]);
    if (s_clip) hal_audio_close(s_clip);
    s_clip = hal_audio_open(path);
    hal_audio_set_rate(s_clip, s_rate);
    hal_audio_play(s_clip);
    s_playing = true;
    s_track_idx = file_i;
    strip_ext(s_files[file_i], s_track, sizeof(s_track));
    s_mode = LIB_PLAYING;
}

static void enter(void)
{
    if (s_sel < s_nfolders) {
        char sub[320];
        snprintf(sub, sizeof(sub), "%s/%s", s_dir, s_folders[s_sel]);
        load_dir(sub);
    } else {
        play_index(s_sel - s_nfolders);
    }
}

static void go_up_dir(void)
{
    if (strcmp(s_dir, LIB_ROOT) == 0) { scene_switch(SCENE_HOME); return; }
    char parent[256];
    snprintf(parent, sizeof(parent), "%s", s_dir);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    load_dir(parent);
}

// --- browse view ---------------------------------------------------------
static void render_browse(Canvas *c)
{
    // Title = current folder name (or "Library" at root).
    const char *slash = strrchr(s_dir, '/');
    const char *title = (strcmp(s_dir, LIB_ROOT) == 0) ? "Library"
                                                       : (slash ? slash + 1 : s_dir);
    theme_header(c, title, THEME_TITLE, NULL, THEME_DIM);

    int n = entry_count();
    if (n == 0) {
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 - 4, &font_small,
                                  "Empty folder", THEME_DIM);
        theme_hint(c, "BACK up");
        return;
    }

    for (int r = 0; r < VIS && s_scroll + r < n; r++) {
        int i = s_scroll + r, y = LIST_TOP + r * ROW_H;
        bool sel = (i == s_sel), is_folder = (i < s_nfolders);
        if (sel) theme_sel_block(c, 0, y, CANVAS_WIDTH, ROW_H - 1);
        color_t col = sel ? THEME_SEL_TEXT : THEME_TEXT;
        theme_icon(c, 10, y + 4, is_folder ? ICON_FOLDER : ICON_NOTE,
                   sel ? THEME_SEL_TEXT : THEME_LABEL, sel ? THEME_SEL_BG : THEME_BG);
        char name[64];
        if (is_folder) snprintf(name, sizeof(name), "%s", s_folders[i]);
        else           strip_ext(s_files[i - s_nfolders], name, sizeof(name));
        font_draw_string(c, 28, y + 5, &font_small, name, col);
    }

    // now-playing strip at the bottom if something is loaded
    if (s_track_idx >= 0) {
        char np[72];
        snprintf(np, sizeof(np), "%s %s", s_playing ? ">" : "||", s_track);
        theme_hint(c, np);
    } else {
        theme_hint(c, "UP/DN move   SEL open   BACK up");
    }
}

// --- now-playing view ----------------------------------------------------
static void mmss(uint32_t ms, char *b, int n)
{
    uint32_t s = ms / 1000;
    snprintf(b, n, "%u:%02u", s / 60, s % 60);
}

static void render_playing(Canvas *c)
{
    theme_header(c, "Now Playing", THEME_TITLE, NULL, THEME_DIM);

    font_draw_string_centered(c, 96, &font_medium, s_track, THEME_TEXT);

    uint32_t pos = s_clip ? hal_audio_pos_ms(s_clip) : 0;
    uint32_t len = s_clip ? hal_audio_len_ms(s_clip) : 0;
    float frac = len ? (float)pos / (float)len : 0.f;
    int bx = 40, bw = CANVAS_WIDTH - 80, by = 150;
    canvas_progress_bar(c, bx, by, bw, 5, frac, THEME_ACCENT, THEME_GRID);

    char te[12], tl[12];
    mmss(pos, te, sizeof(te)); mmss(len, tl, sizeof(tl));
    font_draw_string(c, bx, by + 10, &font_tiny, te, THEME_DIM);
    font_draw_string_right(c, bx + bw, by + 10, &font_tiny, tl, THEME_DIM);

    char st[32];
    snprintf(st, sizeof(st), "%s    %.2fx", s_playing ? "> PLAYING" : "|| PAUSED", s_rate);
    font_draw_string_centered(c, 196, &font_small, st,
                              s_playing ? THEME_ACTIVE : THEME_LABEL);

    theme_hint(c, "SEL play/pause   ENC speed   <> track   BACK list");
}

static void skip(int dir)
{
    int next = s_track_idx + dir;
    if (next < 0 || next >= s_nfiles) return;
    play_index(next);
}

static void on_tick(uint32_t dt)
{
    (void)dt;
    // Auto-advance to the next track when one finishes (playlist feel).
    if (s_mode == LIB_PLAYING && s_playing && s_clip &&
        !hal_audio_is_playing(s_clip) && hal_audio_pos_ms(s_clip) > 0) {
        if (s_track_idx + 1 < s_nfiles) play_index(s_track_idx + 1);
        else s_playing = false;
    }
}

static void on_enter(void)
{
    if (s_dir[0] == 0) load_dir(LIB_ROOT);
    if (s_track_idx >= 0) { /* keep last view? default to browse */ }
    s_mode = LIB_BROWSE;
}

static InputAccel s_accel;   // hold-to-scroll ramp for long folders

static void move(int dir)
{
    int n = entry_count();
    if (n == 0) return;
    s_sel += dir * input_accel_step(&s_accel, dir, plat_millis());
    if (s_sel < 0) s_sel = 0;
    if (s_sel >= n) s_sel = n - 1;
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + VIS) s_scroll = s_sel - VIS + 1;
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    if (s_mode == LIB_PLAYING) render_playing(c);
    else render_browse(c);
}

static void on_input(InputEvent e)
{
    if (s_mode == LIB_PLAYING) {
        switch (e.type) {
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH:
        case INPUT_BTN_PLAY:
            s_playing = !s_playing;
            if (s_playing) hal_audio_play(s_clip); else hal_audio_pause(s_clip);
            break;
        case INPUT_ENC_CW:   s_rate += 0.05f; if (s_rate > 2.0f) s_rate = 2.0f; hal_audio_set_rate(s_clip, s_rate); break;
        case INPUT_ENC_CCW:  s_rate -= 0.05f; if (s_rate < 0.5f) s_rate = 0.5f; hal_audio_set_rate(s_clip, s_rate); break;
        case INPUT_NAV_RIGHT:
        case INPUT_NAV_DOWN:  skip(+1); break;
        case INPUT_NAV_LEFT:
        case INPUT_NAV_UP:    skip(-1); break;
        case INPUT_BTN_BACK:  s_mode = LIB_BROWSE; break;
        case INPUT_BTN_MENU:  scene_switch(SCENE_HOME); break;
        default: break;
        }
        return;
    }

    // Browse mode.
    switch (e.type) {
    case INPUT_NAV_UP:
    case INPUT_ENC_CCW:  move(-1); break;
    case INPUT_NAV_DOWN:
    case INPUT_ENC_CW:   move(+1); break;
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH: enter(); break;
    case INPUT_BTN_PLAY:
        if (s_track_idx >= 0) s_mode = LIB_PLAYING;   // jump to now-playing
        break;
    case INPUT_BTN_BACK: go_up_dir(); break;
    case INPUT_BTN_MENU: scene_switch(SCENE_HOME); break;
    default: break;
    }
}

static const SceneCallbacks CB = {
    .on_enter = on_enter,
    .on_render = on_render,
    .on_input = on_input,
    .on_tick = on_tick,
};

void scene_library_register(void) { scene_register(SCENE_LIBRARY, &CB); }
