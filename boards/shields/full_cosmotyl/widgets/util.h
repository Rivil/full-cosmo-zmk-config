/*
 * SPDX-License-Identifier: MIT
 *
 * Adapted from app/boards/shields/nice_view/widgets/util.h (ZMK, v0.3.0).
 */

#pragma once

#include <lvgl.h>
#include <zmk/endpoints.h>

#define NICEVIEW_PROFILE_COUNT 5
#define CANVAS_SIZE 68

/* nice_view uses white foreground / black background when inverted is off. Keep
 * the same semantics so the stock bolt.c works without modification. */
#define LVGL_BACKGROUND lv_color_white()
#define LVGL_FOREGROUND lv_color_black()

struct status_state {
    /* Central (left) battery */
    uint8_t battery;
    bool charging;

    /* Peripheral (right) battery. Valid once the central has received a split
     * battery event; until then peripheral_battery_valid stays false and the
     * widget renders "--". */
    uint8_t peripheral_battery;
    bool peripheral_battery_valid;

    /* Output status */
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;

    /* Active layer */
    uint8_t layer_index;
    const char *layer_label;

    /* HID LED indicators (bit flags per USB HID keyboard LED report) */
    uint8_t hid_indicators;

    /* True while the keyboard is in ACTIVE activity state. When it drops to
     * IDLE / SLEEP we blank the canvases so the display looks powered-off. */
    bool active;
};

struct battery_status_state {
    uint8_t level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    bool usb_present;
#endif
};

struct peripheral_battery_state {
    uint8_t source;
    uint8_t level;
};

void rotate_canvas(lv_obj_t *canvas, lv_color_t cbuf[]);
void draw_battery_icon(lv_obj_t *canvas, int x, int y, uint8_t level, bool charging);
void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align);
void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color);
void init_line_dsc(lv_draw_line_dsc_t *line_dsc, lv_color_t color, uint8_t width);
void init_arc_dsc(lv_draw_arc_dsc_t *arc_dsc, lv_color_t color, uint8_t width);
