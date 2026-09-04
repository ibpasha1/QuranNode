#include "scene.h"
#include "plat.h"
#include <string.h>

static const char *TAG = "SCENE";

static SceneCallbacks s_scenes[SCENE_COUNT];
static SceneID s_current = SCENE_HOME;
static SceneID s_return  = SCENE_COUNT;   // SCENE_COUNT = none pending

// Each scene provides a registrar; scene_init calls them so the registry is
// populated without a central table of externs to keep in sync.
void scene_home_register(void);
void scene_reader_register(void);
void scene_loop_register(void);
void scene_nav_register(void);
void scene_library_register(void);
void scene_settings_register(void);

void scene_register(SceneID id, const SceneCallbacks *cb)
{
    if (id < SCENE_COUNT && cb) s_scenes[id] = *cb;
}

void scene_init(void)
{
    memset(s_scenes, 0, sizeof(s_scenes));

    scene_home_register();
    scene_reader_register();
    scene_loop_register();
    scene_nav_register();
    scene_library_register();
    scene_settings_register();

    s_current = SCENE_HOME;
    if (s_scenes[s_current].on_enter) s_scenes[s_current].on_enter();
    QN_LOGI(TAG, "scene system ready, current=HOME");
}

void scene_switch(SceneID id)
{
    if (id >= SCENE_COUNT || id == s_current) return;
    if (s_scenes[s_current].on_exit) s_scenes[s_current].on_exit();
    s_current = id;
    if (s_scenes[s_current].on_enter) s_scenes[s_current].on_enter();
}

SceneID scene_get_current(void) { return s_current; }

void scene_render(Canvas *c)
{
    if (s_scenes[s_current].on_render) s_scenes[s_current].on_render(c);
}

void scene_handle_input(InputEvent event)
{
    if (s_scenes[s_current].on_input) s_scenes[s_current].on_input(event);
}

void scene_tick(uint32_t dt_ms)
{
    if (s_scenes[s_current].on_tick) s_scenes[s_current].on_tick(dt_ms);
}

void scene_set_return(SceneID id) { s_return = id; }

SceneID scene_take_return(void)
{
    SceneID r = s_return;
    s_return = SCENE_COUNT;
    return r;
}
