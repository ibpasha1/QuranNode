#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "canvas.h"
#include "theme.h"          // UiIcon
#include "input.h"

#define MENU_MAX_ITEMS     32
#define MENU_ITEM_HEIGHT   15   // compact rows (font_tiny) — matches the tree menu
#define MENU_TITLE_AREA    22   // header height (px)
#define MENU_BOTTOM_MARGIN 16   // space for toast/hints (px)
#define MENU_VISIBLE_ITEMS ((CANVAS_HEIGHT - MENU_TITLE_AREA - MENU_BOTTOM_MARGIN) / MENU_ITEM_HEIGHT)

typedef enum {
    MENU_ITEM_ACTION,
    MENU_ITEM_TOGGLE,
    MENU_ITEM_SLIDER,
    MENU_ITEM_SUBMENU,
    MENU_ITEM_VALUE,
    MENU_ITEM_HEADER,   // non-selectable group header (bold, no cursor)
} MenuItemType;

typedef struct MenuItem {
    const char *label;
    const char *value;
    MenuItemType type;
    void (*on_select)(struct MenuItem *item);
    int data;
    int min_val;
    int max_val;
    UiIcon icon;        // ICON_NONE = auto-pick from type
    const char *desc;   // one-line description for the Board B peek
} MenuItem;

typedef struct {
    MenuItem items[MENU_MAX_ITEMS];
    uint8_t count;
    uint8_t selected;
    bool editing;
    float scroll_y;
    float indicator_y;
    const char *title;
    const char *toast_msg;      // temporary notification text
    uint8_t toast_frames;       // frames remaining for toast display
    int last_peek_sel;          // last selection forwarded to Board B (-1 = none)
} Menu;

void menu_show_toast(Menu *m, const char *msg);

void menu_init(Menu *m, const char *title);
void menu_add_item(Menu *m, const char *label, MenuItemType type,
                   void (*on_select)(MenuItem *item));
void menu_render(Menu *m, Canvas *c);
void menu_handle_input(Menu *m, InputEvent event);
void menu_set_item_value(Menu *m, uint8_t index, const char *value);
void menu_set_item_data(Menu *m, uint8_t index, int data, int min_v, int max_v);
void menu_set_item_icon(Menu *m, uint8_t index, UiIcon icon);
void menu_set_item_desc(Menu *m, uint8_t index, const char *desc);
