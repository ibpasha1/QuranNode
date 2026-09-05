#include "menu.h"
#include "font.h"
#include "theme.h"
#include "tween.h"
#include "plat.h"
#include "input_accel.h"
#include "hal.h"
#include <string.h>
#include <stdio.h>

// UI click feedback: a short tick through the audio HAL (accent = confirm).
static inline void menu_click(bool accent) { hal_audio_click(accent); }

// Hold-to-scroll ramp. Shared across Menu instances — only one menu receives
// input at a time, and a scene switch is a >250ms gap that resets it anyway.
static InputAccel s_accel;

// Default icon from item type when none is set explicitly.
static UiIcon menu_type_icon(const MenuItem *it)
{
    if (it->icon != ICON_NONE) return it->icon;
    switch (it->type) {
    case MENU_ITEM_TOGGLE:  return ICON_TOGGLE;
    case MENU_ITEM_SLIDER:  return ICON_SLIDER;
    case MENU_ITEM_VALUE:   return ICON_INFO;
    case MENU_ITEM_SUBMENU: return ICON_FOLDER;
    case MENU_ITEM_HEADER:  return ICON_NONE;
    default:                return ICON_FX;   // ACTION
    }
}

void menu_init(Menu *m, const char *title)
{
    memset(m, 0, sizeof(Menu));
    m->title = title;
    m->scroll_y = 0;
    m->indicator_y = 0;
    m->toast_msg = NULL;
    m->toast_frames = 0;
    m->last_peek_sel = -1;
}

void menu_set_item_desc(Menu *m, uint8_t index, const char *desc)
{
    if (index < m->count) m->items[index].desc = desc;
}

void menu_show_toast(Menu *m, const char *msg)
{
    m->toast_msg = msg;
    m->toast_frames = 60;  // ~2 seconds at 30fps
}

void menu_add_item(Menu *m, const char *label, MenuItemType type,
                   void (*on_select)(MenuItem *item))
{
    if (m->count >= MENU_MAX_ITEMS) return;
    MenuItem *item = &m->items[m->count];
    item->label = label;
    item->value = NULL;
    item->type = type;
    item->on_select = on_select;
    item->data = 0;
    item->min_val = 0;
    item->max_val = 100;
    item->icon = ICON_NONE;
    item->desc = NULL;
    m->count++;
}

void menu_set_item_icon(Menu *m, uint8_t index, UiIcon icon)
{
    if (index < m->count) m->items[index].icon = icon;
}

void menu_set_item_value(Menu *m, uint8_t index, const char *value)
{
    if (index < m->count) {
        m->items[index].value = value;
    }
}

void menu_set_item_data(Menu *m, uint8_t index, int data, int min_v, int max_v)
{
    if (index < m->count) {
        m->items[index].data = data;
        m->items[index].min_val = min_v;
        m->items[index].max_val = max_v;
    }
}

void menu_render(Menu *m, Canvas *c)
{
    // Compact file-system rows to match the tree menu: font_tiny, header rail.
    const int right_x = CANVAS_WIDTH - 10;
    const int start_y = 22;

    if (m->title) theme_header(c, m->title, THEME_TITLE, NULL, THEME_DIM);

    int visible_start = (int)(m->scroll_y / MENU_ITEM_HEIGHT);
    if (visible_start < 0) visible_start = 0;
    int max_visible = MENU_VISIBLE_ITEMS;

    for (int i = 0; i < m->count && i < max_visible + 1; i++) {
        int idx = visible_start + i;
        if (idx >= m->count) break;

        int y = start_y + i * MENU_ITEM_HEIGHT - ((int)m->scroll_y % MENU_ITEM_HEIGHT);
        if (y < start_y - MENU_ITEM_HEIGHT || y > CANVAS_HEIGHT - MENU_BOTTOM_MARGIN) continue;

        MenuItem *it = &m->items[idx];
        bool is_header = (it->type == MENU_ITEM_HEADER);
        bool is_sel = (idx == m->selected) && !is_header;
        if (is_sel) theme_sel_block(c, 0, y, CANVAS_WIDTH, MENU_ITEM_HEIGHT - 1);

        color_t text_color = is_sel ? THEME_SEL_TEXT : (is_header ? THEME_TITLE : THEME_TEXT);
        color_t bg = is_sel ? THEME_SEL_BG : THEME_BG;

        int label_x = 10;
        if (!is_header) {
            theme_icon(c, 10, y + 3, menu_type_icon(it),
                       is_sel ? THEME_SEL_TEXT : THEME_LABEL, bg);
            label_x = 26;
        } else {
            canvas_hline(c, 8, y + MENU_ITEM_HEIGHT - 3, CANVAS_WIDTH - 16, THEME_GRID);
        }
        font_draw_string(c, label_x, y + 4, &font_tiny, it->label, text_color);

        if (is_header) {
            /* header has no value column */
        } else if (it->value) {
            color_t val_color = is_sel ? THEME_SEL_TEXT : THEME_ACCENT;
            font_draw_string_right(c, right_x, y + 4, &font_tiny, it->value, val_color);
        } else if (it->type == MENU_ITEM_TOGGLE) {
            const char *state = it->data ? "ON" : "OFF";
            color_t color = is_sel ? THEME_SEL_TEXT : (it->data ? THEME_ACTIVE : THEME_DIM);
            font_draw_string_right(c, right_x, y + 4, &font_tiny, state, color);
        } else if (it->type == MENU_ITEM_SLIDER) {
            bool is_editing = (idx == m->selected && m->editing);
            int bar_w = 40, bar_x = right_x - bar_w;
            float progress = (float)(it->data - it->min_val) / (float)(it->max_val - it->min_val);
            color_t bar_fg = is_editing ? THEME_ACTIVE : THEME_BAR;
            color_t bar_bg = is_sel ? THEME_ROW : THEME_GRID;
            canvas_progress_bar(c, bar_x, y + 5, bar_w, 4, progress, bar_fg, bar_bg);
            char val_buf[8];
            snprintf(val_buf, sizeof(val_buf), "%d", it->data);
            color_t num_color = is_editing ? THEME_ACTIVE : (is_sel ? THEME_SEL_TEXT : THEME_TEXT);
            font_draw_string_right(c, bar_x - 4, y + 4, &font_tiny, val_buf, num_color);
        } else if (it->type == MENU_ITEM_SUBMENU) {
            font_draw_string_right(c, right_x, y + 4, &font_tiny, ">",
                                   is_sel ? THEME_SEL_TEXT : THEME_DIM);
        }
    }

    // Scrollbar
    if (m->count > max_visible) {
        int bar_height = max_visible * MENU_ITEM_HEIGHT;
        int thumb_h = (max_visible * bar_height) / m->count;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = start_y + (m->selected * (bar_height - thumb_h)) / (m->count - 1);
        canvas_rect_fill(c, CANVAS_WIDTH - 3, thumb_y, 3, thumb_h, THEME_ACCENT);
    }

    // Toast notification overlay
    if (m->toast_frames > 0) {
        m->toast_frames--;
        int tw = font_string_width(&font_small, m->toast_msg) + 20;
        int tx = (CANVAS_WIDTH - tw) / 2;
        int ty = CANVAS_HEIGHT - 40;
        canvas_rect_fill(c, tx, ty, tw, 24, THEME_PANEL);
        canvas_rect(c, tx, ty, tw, 24, COLOR_GREEN);
        font_draw_string_centered(c, ty + 5, &font_small, m->toast_msg, COLOR_GREEN);
    }

    // Bottom hint rail (matches the tree menu).
    theme_hint(c, m->editing ? "L/R adjust   SEL done"
                             : "UP/DN move   SEL open/edit   BACK up");
}

static void menu_apply_scroll(Menu *m)
{
    float target_scroll = (m->selected > MENU_VISIBLE_ITEMS - 1)
        ? (float)((m->selected - MENU_VISIBLE_ITEMS + 1) * MENU_ITEM_HEIGHT) : 0;
    tween_start(&m->scroll_y, m->scroll_y, target_scroll, 150, EASE_OUT_CUBIC, NULL, NULL);
}

static void menu_move_down(Menu *m)
{
    int n = m->selected;
    while (n < m->count - 1) {                 // skip non-selectable headers
        n++;
        if (m->items[n].type != MENU_ITEM_HEADER) break;
    }
    if (n != m->selected && m->items[n].type != MENU_ITEM_HEADER) {
        m->editing = false;
        m->selected = n;
        menu_apply_scroll(m);
        menu_click(false);
    }
}

static void menu_move_up(Menu *m)
{
    int n = m->selected;
    while (n > 0) {
        n--;
        if (m->items[n].type != MENU_ITEM_HEADER) break;
    }
    if (n != m->selected && m->items[n].type != MENU_ITEM_HEADER) {
        m->editing = false;
        m->selected = n;
        menu_apply_scroll(m);
        menu_click(false);
    }
}

static void menu_adjust_value(Menu *m, int dir)
{
    if (!m->editing) return;
    if (m->items[m->selected].type != MENU_ITEM_SLIDER) return;

    m->items[m->selected].data += dir;
    if (m->items[m->selected].data > m->items[m->selected].max_val) {
        m->items[m->selected].data = m->items[m->selected].max_val;
    }
    if (m->items[m->selected].data < m->items[m->selected].min_val) {
        m->items[m->selected].data = m->items[m->selected].min_val;
    }
    if (m->items[m->selected].on_select) {
        m->items[m->selected].on_select(&m->items[m->selected]);
    }
}

static void menu_select_item(Menu *m)
{
    if (m->items[m->selected].type == MENU_ITEM_HEADER) return;
    menu_click(true);
    if (m->items[m->selected].type == MENU_ITEM_SLIDER) {
        m->editing = !m->editing;
    } else {
        if (m->items[m->selected].type == MENU_ITEM_TOGGLE) {
            m->items[m->selected].data = !m->items[m->selected].data;
        }
        if (m->items[m->selected].on_select) {
            m->items[m->selected].on_select(&m->items[m->selected]);
        }
    }
}

void menu_handle_input(Menu *m, InputEvent event)
{
    if (event.type == INPUT_BTN_BACK) {
        if (m->editing) {
            m->editing = false;
        }
        return;
    }

    switch (event.type) {
    // 5-way navigation (held keys repeat; the accel ramp turns that into
    // progressively larger jumps through long lists)
    case INPUT_NAV_DOWN:
        if (m->editing) {
            menu_adjust_value(m, -1);
        } else {
            int step = input_accel_step(&s_accel, +1, plat_millis());
            while (step--) menu_move_down(m);
        }
        break;
    case INPUT_NAV_UP:
        if (m->editing) {
            menu_adjust_value(m, +1);
        } else {
            int step = input_accel_step(&s_accel, -1, plat_millis());
            while (step--) menu_move_up(m);
        }
        break;
    case INPUT_NAV_RIGHT:
        menu_adjust_value(m, +1);
        break;
    case INPUT_NAV_LEFT:
        menu_adjust_value(m, -1);
        break;
    case INPUT_NAV_SELECT:
        menu_select_item(m);
        break;

    // Encoder input (from Board B or legacy)
    case INPUT_ENC_CW:
        if (m->editing) {
            menu_adjust_value(m, +1);
        } else {
            menu_move_down(m);
        }
        break;
    case INPUT_ENC_CCW:
        if (m->editing) {
            menu_adjust_value(m, -1);
        } else {
            menu_move_up(m);
        }
        break;
    case INPUT_ENC_PUSH:
        menu_select_item(m);
        break;

    default:
        break;
    }
}
