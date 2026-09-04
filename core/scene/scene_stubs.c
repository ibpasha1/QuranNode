// scene_stubs.c — placeholder scenes for features arriving in later M1 steps.
//
// Ayah Loop (M1b), Navigation (M1c), Library (M1d) and Settings each get a real
// implementation in their own file later; for now they render a titled "coming
// soon" panel and return home on BACK, so the scene graph and navigation are
// wired and walkable from M0.
#include "scene.h"
#include "theme.h"
#include "font.h"
#include <stddef.h>   // NULL

static void stub_render(Canvas *c, const char *title, const char *note)
{
    theme_clear(c);
    theme_header(c, title, THEME_TITLE, NULL, THEME_DIM);
    font_draw_string_centered(c, CANVAS_HEIGHT / 2 - 4, &font_small, note, THEME_DIM);
    theme_hint(c, "BACK home");
}

static void stub_input(InputEvent e)
{
    if (e.type == INPUT_BTN_BACK || e.type == INPUT_NAV_SELECT ||
        e.type == INPUT_BTN_MENU) {
        scene_switch(SCENE_HOME);
    }
}

#define STUB_SCENE(fn, id, title, note)                              \
    static void fn##_render(Canvas *c) { stub_render(c, title, note); } \
    static const SceneCallbacks fn##_cb = {                          \
        .on_render = fn##_render, .on_input = stub_input,            \
    };                                                               \
    void fn##_register(void) { scene_register(id, &fn##_cb); }

