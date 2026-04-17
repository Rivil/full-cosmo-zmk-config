/*
 * SPDX-License-Identifier: MIT
 *
 * Custom nice!view status widget for full_cosmotyl, central side only.
 *
 * Layout (top-to-bottom on the mounted display):
 *   Top canvas    : selected endpoint + active profile, left battery, right battery
 *                   (each battery shows a glyph, %, and a lightning bolt while USB
 *                    power is present on the central).
 *   Middle canvas : active layer name (large, centered).
 *   Bottom canvas : LED lock indicators — CAPS / NUM / SCR — each drawn only
 *                   while the host has the matching LED on.
 *
 * Each 68x68 canvas is drawn with horizontal baseline and then rotated 90° CW
 * by rotate_canvas() into its final vertical slot. x/y below are in canvas
 * (pre-rotation) coordinates.
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

#include "status.h"

/* USB HID keyboard LED report bits */
#define HID_LED_NUM    0x01
#define HID_LED_CAPS   0x02
#define HID_LED_SCROLL 0x04

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

struct hid_indicators_state {
    uint8_t indicators;
};

/* ---------- drawing ---------- */

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);

    lv_draw_label_dsc_t label_small;
    init_label_dsc(&label_small, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t label_right;
    init_label_dsc(&label_right, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    /* Row 1: output status + profile number */
    char output_text[10] = {};
    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        strcat(output_text, LV_SYMBOL_USB);
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            strcat(output_text, state->active_profile_connected ? LV_SYMBOL_WIFI
                                                                : LV_SYMBOL_CLOSE);
        } else {
            strcat(output_text, LV_SYMBOL_SETTINGS);
        }
        break;
    }
    lv_canvas_draw_text(canvas, 0, 0, 20, &label_small, output_text);

    if (state->selected_endpoint.transport == ZMK_TRANSPORT_BLE) {
        char prof[4];
        snprintf(prof, sizeof(prof), "%d", state->active_profile_index + 1);
        lv_canvas_draw_text(canvas, CANVAS_SIZE - 20, 0, 18, &label_right, prof);
    }

    /* Row 2: left (central) battery.
     *
     * nice_nano_v2 doesn't route the charger STAT pin to a readable GPIO, so
     * `charging` really means "USB bus is providing power". That's a good-
     * enough proxy for everyday use but can't distinguish "actively charging"
     * from "topped off on trickle". We split it by level threshold: when USB
     * is present and the cell reads full, show "FULL" instead of the bolt;
     * while climbing, show the bolt; on battery, just show the percentage. */
    bool usb_full = state->charging && state->battery >= 97;
    bool usb_charging = state->charging && !usb_full;
    draw_battery_icon(canvas, 0, 22, state->battery, usb_charging);
    char lbat[6];
    if (usb_full) {
        snprintf(lbat, sizeof(lbat), "FULL");
    } else {
        snprintf(lbat, sizeof(lbat), "%3d%%", state->battery);
    }
    lv_canvas_draw_text(canvas, 36, 22, 32, &label_small, lbat);

    /* Row 3: right (peripheral) battery */
    draw_battery_icon(canvas, 0, 44,
                      state->peripheral_battery_valid ? state->peripheral_battery : 0, false);
    char rbat[6];
    if (state->peripheral_battery_valid) {
        snprintf(rbat, sizeof(rbat), "%3d%%", state->peripheral_battery);
    } else {
        snprintf(rbat, sizeof(rbat), " --");
    }
    lv_canvas_draw_text(canvas, 36, 44, 32, &label_small, rbat);

    rotate_canvas(canvas, cbuf);
}

static void draw_middle(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_big;
    init_label_dsc(&label_big, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    char text[12];
    if (state->layer_label == NULL || strlen(state->layer_label) == 0) {
        snprintf(text, sizeof(text), "L%u", state->layer_index);
    } else {
        strncpy(text, state->layer_label, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
    }
    lv_canvas_draw_text(canvas, 0, 24, CANVAS_SIZE, &label_big, text);

    rotate_canvas(canvas, cbuf);
}

static void draw_bottom(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 2);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_mid;
    init_label_dsc(&label_mid, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    if (state->hid_indicators & HID_LED_CAPS) {
        lv_canvas_draw_text(canvas, 0, 4, CANVAS_SIZE, &label_mid, "CAPS");
    }
    if (state->hid_indicators & HID_LED_NUM) {
        lv_canvas_draw_text(canvas, 0, 24, CANVAS_SIZE, &label_mid, "NUM");
    }
    if (state->hid_indicators & HID_LED_SCROLL) {
        lv_canvas_draw_text(canvas, 0, 44, CANVAS_SIZE, &label_mid, "SCR");
    }

    rotate_canvas(canvas, cbuf);
}

/* ---------- central battery + USB (charging proxy) ---------- */

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif
    widget->state.battery = state.level;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

/* ---------- peripheral battery (right half) ---------- */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

static void set_peripheral_battery_status(struct zmk_widget_status *widget,
                                          struct peripheral_battery_state state) {
    widget->state.peripheral_battery = state.level;
    widget->state.peripheral_battery_valid = true;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void peripheral_battery_update_cb(struct peripheral_battery_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_peripheral_battery_status(widget, state);
    }
}

static struct peripheral_battery_state peripheral_battery_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    return (struct peripheral_battery_state){
        .source = ev ? ev->source : 0,
        .level = ev ? ev->state_of_charge : 0,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_battery, struct peripheral_battery_state,
                            peripheral_battery_update_cb, peripheral_battery_get_state)
ZMK_SUBSCRIPTION(widget_peripheral_battery, zmk_peripheral_battery_state_changed);

#endif /* CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING */

/* ---------- output / endpoint ---------- */

static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

/* ---------- layer ---------- */

static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index,
        .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

/* ---------- HID indicators ---------- */

static void set_hid_indicators_status(struct zmk_widget_status *widget,
                                      struct hid_indicators_state state) {
    widget->state.hid_indicators = state.indicators;
    draw_bottom(widget->obj, widget->cbuf3, &widget->state);
}

static void hid_indicators_update_cb(struct hid_indicators_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_hid_indicators_status(widget, state);
    }
}

static struct hid_indicators_state hid_indicators_get_state(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    return (struct hid_indicators_state){
        .indicators = ev ? ev->indicators : zmk_hid_indicators_get_current_profile(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_hid_indicators_status, struct hid_indicators_state,
                            hid_indicators_update_cb, hid_indicators_get_state)
ZMK_SUBSCRIPTION(widget_hid_indicators_status, zmk_hid_indicators_changed);

/* ---------- init ---------- */

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, 24, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, -44, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    widget_peripheral_battery_init();
#endif
    widget_output_status_init();
    widget_layer_status_init();
    widget_hid_indicators_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
