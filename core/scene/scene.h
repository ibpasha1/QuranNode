// scene.h — QuranNode scene (screen) state machine.
//
// Same lightweight pattern as DSP-Mini: each screen is a set of stateless
// callbacks registered against a SceneID; exactly one scene is active. app.c
// drives scene_render() every frame and scene_handle_input() per event. Kept
// intentionally small — no dual-board companion logic, no deep-edit sessions.
#pragma once

#include "canvas.h"
#include "input.h"

typedef enum {
    SCENE_HOME = 0,   // calm home: next prayer, "continue reading", today's progress
    SCENE_READER,     // the focus reader (centered ayah + synced recitation)
    SCENE_LOOP,       // ayah-loop / repeat routine editor (hifz)
    SCENE_NAV,        // Surah > Juz > Page > Ayah jump
    SCENE_LIBRARY,    // local audio player (nasheeds / lectures / audiobooks)
    SCENE_SETTINGS,   // preferences
    SCENE_COUNT,
} SceneID;

typedef struct {
    void (*on_enter)(void);
    void (*on_exit)(void);
    void (*on_render)(Canvas *c);
    void (*on_input)(InputEvent event);
    void (*on_tick)(uint32_t dt_ms);   // optional: per-frame time advance
} SceneCallbacks;

// Register the built-in scenes and switch to the initial scene.
void scene_init(void);

// Register/replace a scene's callbacks (called by each scene's registrar).
void scene_register(SceneID id, const SceneCallbacks *cb);

void scene_switch(SceneID id);
SceneID scene_get_current(void);

void scene_render(Canvas *c);
void scene_handle_input(InputEvent event);
void scene_tick(uint32_t dt_ms);

// One-shot "return here on next BACK" target (e.g. dive into NAV then come back
// to the reader). Cleared after use.
void scene_set_return(SceneID id);
SceneID scene_take_return(void);   // returns SCENE_COUNT if none pending
