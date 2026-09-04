#pragma once

#include <stdint.h>
#include "canvas.h"

typedef struct {
    const uint16_t *data;   // RGB565 pixel data
    uint16_t width;
    uint16_t height;
} Sprite;

typedef struct {
    const Sprite *frames;
    uint8_t frame_count;
    uint8_t current_frame;
    uint16_t frame_duration_ms;
    uint16_t elapsed_ms;
    bool loop;
    bool playing;
} AnimatedSprite;

void sprite_draw(Canvas *c, int x, int y, const Sprite *s);
void sprite_draw_tinted(Canvas *c, int x, int y, const Sprite *s, color_t tint);

void anim_sprite_init(AnimatedSprite *as, const Sprite *frames, uint8_t count,
                      uint16_t frame_ms, bool loop);
void anim_sprite_update(AnimatedSprite *as, uint16_t dt_ms);
void anim_sprite_draw(Canvas *c, int x, int y, const AnimatedSprite *as);
void anim_sprite_play(AnimatedSprite *as);
void anim_sprite_stop(AnimatedSprite *as);
void anim_sprite_reset(AnimatedSprite *as);
