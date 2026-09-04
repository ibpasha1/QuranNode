#include "sprite.h"

void sprite_draw(Canvas *c, int x, int y, const Sprite *s)
{
    for (int sy = 0; sy < s->height; sy++) {
        for (int sx = 0; sx < s->width; sx++) {
            color_t pixel = s->data[sy * s->width + sx];
            if (pixel != 0) {  // 0 = transparent
                canvas_pixel(c, x + sx, y + sy, pixel);
            }
        }
    }
}

void sprite_draw_tinted(Canvas *c, int x, int y, const Sprite *s, color_t tint)
{
    for (int sy = 0; sy < s->height; sy++) {
        for (int sx = 0; sx < s->width; sx++) {
            color_t pixel = s->data[sy * s->width + sx];
            if (pixel != 0) {
                canvas_pixel(c, x + sx, y + sy, tint);
            }
        }
    }
}

void anim_sprite_init(AnimatedSprite *as, const Sprite *frames, uint8_t count,
                      uint16_t frame_ms, bool loop)
{
    as->frames = frames;
    as->frame_count = count;
    as->current_frame = 0;
    as->frame_duration_ms = frame_ms;
    as->elapsed_ms = 0;
    as->loop = loop;
    as->playing = false;
}

void anim_sprite_update(AnimatedSprite *as, uint16_t dt_ms)
{
    if (!as->playing) return;

    as->elapsed_ms += dt_ms;
    while (as->elapsed_ms >= as->frame_duration_ms) {
        as->elapsed_ms -= as->frame_duration_ms;
        as->current_frame++;
        if (as->current_frame >= as->frame_count) {
            if (as->loop) {
                as->current_frame = 0;
            } else {
                as->current_frame = as->frame_count - 1;
                as->playing = false;
                return;
            }
        }
    }
}

void anim_sprite_draw(Canvas *c, int x, int y, const AnimatedSprite *as)
{
    if (as->frame_count > 0) {
        sprite_draw(c, x, y, &as->frames[as->current_frame]);
    }
}

void anim_sprite_play(AnimatedSprite *as)
{
    as->playing = true;
}

void anim_sprite_stop(AnimatedSprite *as)
{
    as->playing = false;
}

void anim_sprite_reset(AnimatedSprite *as)
{
    as->current_frame = 0;
    as->elapsed_ms = 0;
}
