// input.h — QuranNode input event model.
//
// A deliberately small, product-shaped subset of the DSP-Mini input layer. The
// device is driven almost entirely by one rotary encoder + a handful of physical
// keys (back, mode, play/pause, bookmark). Enum member names that the reused menu
// widget depends on (INPUT_NAV_*, INPUT_ENC_*, INPUT_BTN_BACK) are preserved.
//
// The active HAL turns physical events (encoder quadrature, GPIO keys, or — in the
// simulator — the keyboard) into these InputEvents and hands them to app_input().
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    INPUT_NONE = 0,

    // 5-way / D-pad navigation (also produced by keyboard arrows in the sim).
    INPUT_NAV_UP,
    INPUT_NAV_DOWN,
    INPUT_NAV_LEFT,
    INPUT_NAV_RIGHT,
    INPUT_NAV_SELECT,       // center press (short)
    INPUT_NAV_SELECT_LONG,  // center long-press (>500ms)

    // The primary rotary encoder.
    INPUT_ENC_CW,           // turn clockwise (one detent)
    INPUT_ENC_CCW,          // turn counter-clockwise
    INPUT_ENC_PUSH,         // encoder button press (short)
    INPUT_ENC_PUSH_LONG,    // encoder button long-press

    // Dedicated physical keys along the front.
    INPUT_BTN_BACK,         // back / up-one-level
    INPUT_BTN_MODE,         // modifier (hold: encoder becomes volume, etc.)
    INPUT_BTN_PLAY,         // play / pause transport
    INPUT_BTN_BOOKMARK,     // bookmark (short: recent, long: save here)
    INPUT_BTN_MENU,         // Quran/menu home key
    INPUT_BTN_HELP,         // on-screen shortcut overlay

    INPUT_EVENT_COUNT,
} InputEventType;

// Kept for source-compatibility with the reused menu widget (single encoder here).
typedef enum {
    ENC_NAV = 0,
    ENC_COUNT = 1,
} EncoderID;
#define ENC_VOLUME ENC_NAV

typedef struct {
    InputEventType type;
    EncoderID encoder_id;   // always ENC_NAV on this device
    bool pressed;           // press/release flag for key events; edges ignore it
} InputEvent;
