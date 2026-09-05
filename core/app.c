#include "app.h"
#include "scene.h"
#include "player.h"
#include "progress.h"
#include "prefs.h"
#include "theme.h"
#include "tween.h"
#include "plat.h"

void app_init(void)
{
    tween_init();
    progress_init();                        // durable resume point + bookmarks
    player_init(&g_player, "abdulbasit");   // the shared recitation transport
    prefs_init();                           // speed/font/brightness/tajweed (applies them)
    scene_init();   // starts on the calm Home screen
}

void app_tick(uint32_t dt_ms)
{
    tween_update_all((int)dt_ms);
    scene_tick(dt_ms);   // e.g. the reader advances its audio playhead + highlight
}

void app_input(InputEvent ev)
{
    theme_keybar_note(ev.type);   // lights the matching key chip in the keybar
    scene_handle_input(ev);
}

void app_render(Canvas *c)
{
    scene_render(c);
}
