/*
 * SPDX-License-Identifier: MIT
 *
 * Custom nice!view status widget for full_cosmotyl, central side only.
 *
 * Physical layout of the three internal 68x68 canvases:
 *   * Child 0 ("top") at widget (92, 0)  — 68x68 buffer, visible as-is.
 *   * Child 1 ("middle") at widget (24, 0) — 68x68.
 *   * Child 2 ("bottom") at widget (-44, 0) — 68x68 buffer positioned so only
 *     the last 24 canvas columns are inside the 160x68 widget.
 *
 * How this maps to what the user sees on the mounted display:
 *
 *    USER'S TOP     <-- child 2 (only ~24 px tall in the user's vertical)
 *       …
 *    USER'S MIDDLE  <-- child 1 (68 px tall)
 *       …
 *    USER'S BOTTOM  <-- child 0 (68 px tall)
 *
 * Content assignment (driven by user feedback — we want the CAPS/NUM/SCR
 * indicators in the big slot, batteries can live in the thin top strip):
 *   * child 2 (user's top, 24 px):   compact battery readout "L85 R72"
 *   * child 1 (user's middle, 68 px): endpoint+profile (large), layer name
 *   * child 0 (user's bottom, 68 px): HID LED indicators, one per row, big font
 *
 * x/y values in the draw_* functions below are canvas (pre-rotation)
 * coordinates. Each canvas is rotated 90° by rotate_canvas() before it
 * lands on the widget.
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
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

/* Canvas child indices — see header comment. */
#define CANVAS_HID     0
#define CANVAS_MIDDLE  1
#define CANVAS_BAT     2

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

/* Flag propagated from peripheral_battery_get_state so we only mark the
 * peripheral level as "valid" when it came from an actual event. The
 * ZMK_DISPLAY_WIDGET_LISTENER init path calls get_state() with eh=NULL to do
 * the first draw; without this flag we'd render "0%" from that synthetic call
 * before any real reading arrived. */
struct peripheral_battery_event_state {
    uint8_t source;
    uint8_t level;
    bool from_event;
};

/* ---------- drawing ---------- */

/* Formats one battery into a 2-char buffer (plus NUL). Caps at 99 to keep
 * the compact top line under the 68px canvas width — the "+1%" precision lost
 * when the cell is actually at 100 is not worth the extra digit. */
static void format_battery_2(char out[3], uint8_t level, bool valid, bool full) {
    if (!valid) {
        strcpy(out, "--");
    } else if (full) {
        strcpy(out, "FL");
    } else {
        snprintf(out, 3, "%2u", level > 99 ? 99 : level);
    }
}

static void draw_bat(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, CANVAS_BAT);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_small;
    init_label_dsc(&label_small, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    if (!state->active) {
        rotate_canvas(canvas, cbuf);
        return;
    }

    /* nice_nano_v2 doesn't route the charger STAT pin to a GPIO, so
     * state->charging really means "USB is providing power to the board".
     * Under 97% we render a "+" after the left battery as a charging hint;
     * at/above 97% we show "FL" (full). */
    bool full = state->charging && state->battery >= 97;
    bool charging = state->charging && !full;

    char lbuf[3], rbuf[3];
    format_battery_2(lbuf, state->battery, true, full);
    format_battery_2(rbuf, state->peripheral_battery, state->peripheral_battery_valid, false);

    char line[16];
    if (charging) {
        /* "L85+R72" — drop the space between halves to make room for the
         * charging hint while keeping the whole line under 68px. */
        snprintf(line, sizeof(line), "L%s+R%s", lbuf, rbuf);
    } else {
        snprintf(line, sizeof(line), "L%s  R%s", lbuf, rbuf);
    }

    lv_canvas_draw_text(canvas, 0, 5, CANVAS_SIZE, &label_small, line);

    rotate_canvas(canvas, cbuf);
}

static void draw_middle(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, CANVAS_MIDDLE);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_huge;
    init_label_dsc(&label_huge, LVGL_FOREGROUND, &lv_font_montserrat_26, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t label_big;
    init_label_dsc(&label_big, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    if (!state->active) {
        rotate_canvas(canvas, cbuf);
        return;
    }

    /* Top half: endpoint + profile. "USB" when the USB endpoint is selected;
     * "BT n" (1-indexed) for BLE regardless of whether the profile is
     * currently connected. */
    char ep[8];
    if (state->selected_endpoint.transport == ZMK_TRANSPORT_USB) {
        snprintf(ep, sizeof(ep), "USB");
    } else {
        snprintf(ep, sizeof(ep), "BT %d", state->active_profile_index + 1);
    }
    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_huge, ep);

    /* Bottom half: layer name (or "L#" if the keymap didn't label it). */
    char layer[12];
    if (state->layer_label == NULL || strlen(state->layer_label) == 0) {
        snprintf(layer, sizeof(layer), "L%u", state->layer_index);
    } else {
        strncpy(layer, state->layer_label, sizeof(layer) - 1);
        layer[sizeof(layer) - 1] = '\0';
    }
    lv_canvas_draw_text(canvas, 0, 40, CANVAS_SIZE, &label_big, layer);

    rotate_canvas(canvas, cbuf);
}

static void draw_hid(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, CANVAS_HID);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_big;
    init_label_dsc(&label_big, LVGL_FOREGROUND, &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    if (!state->active) {
        rotate_canvas(canvas, cbuf);
        return;
    }

    /* One line per lock. Each ~20 px tall, 22 px stride → 66 px total, fits
     * in 68. Line only drawn when the matching host LED is reported on. */
    if (state->hid_indicators & HID_LED_CAPS) {
        lv_canvas_draw_text(canvas, 0, 2, CANVAS_SIZE, &label_big, "CAPS");
    }
    if (state->hid_indicators & HID_LED_NUM) {
        lv_canvas_draw_text(canvas, 0, 24, CANVAS_SIZE, &label_big, "NUM");
    }
    if (state->hid_indicators & HID_LED_SCROLL) {
        lv_canvas_draw_text(canvas, 0, 46, CANVAS_SIZE, &label_big, "SCR");
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
    draw_bat(widget->obj, widget->cbuf3, &widget->state);
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
                                          struct peripheral_battery_event_state state) {
    /* Ignore level=0 events. ZMK's BAS path on the central side sometimes
     * fires a 0-reading before the peripheral has taken a real sensor sample
     * (or when a stale bond prevents the BAS characteristic from being
     * re-read after discovery). Treating 0 as "no reading yet" avoids
     * sticking the UI on a fake 0% — once a real >0 reading arrives we
     * latch it, and we keep the last real reading across transient 0s. */
    if (state.from_event && state.level > 0) {
        widget->state.peripheral_battery = state.level;
        widget->state.peripheral_battery_valid = true;
    }
    draw_bat(widget->obj, widget->cbuf3, &widget->state);
}

static void peripheral_battery_update_cb(struct peripheral_battery_event_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_peripheral_battery_status(widget, state);
    }
}

static struct peripheral_battery_event_state peripheral_battery_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    return (struct peripheral_battery_event_state){
        .source = ev ? ev->source : 0,
        .level = ev ? ev->state_of_charge : 0,
        .from_event = (ev != NULL),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_battery, struct peripheral_battery_event_state,
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
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
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
    draw_hid(widget->obj, widget->cbuf, &widget->state);
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

/* ---------- activity state (sleep/wake) ---------- */

/* Tracks ZMK's activity state so the widget can blank all three canvases when
 * the keyboard goes IDLE (or SLEEP) and repaint them on return to ACTIVE.
 * This is how we implement a display sleep at the widget level — the stock
 * CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE path can't physically power the panel down
 * on nice_nano_v2 (DISP_EN isn't wired) and would only stop the LVGL tick
 * loop, which we need running so that the wake redraw actually flushes. */
struct activity_status_state {
    bool active;
};

static void set_activity_status(struct zmk_widget_status *widget,
                                struct activity_status_state state) {
    widget->state.active = state.active;
    draw_bat(widget->obj, widget->cbuf3, &widget->state);
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
    draw_hid(widget->obj, widget->cbuf, &widget->state);
}

static void activity_status_update_cb(struct activity_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_activity_status(widget, state); }
}

static struct activity_status_state activity_status_get_state(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    enum zmk_activity_state s = ev ? ev->state : zmk_activity_get_state();
    return (struct activity_status_state){.active = (s == ZMK_ACTIVITY_ACTIVE)};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_activity_status, struct activity_status_state,
                            activity_status_update_cb, activity_status_get_state)
ZMK_SUBSCRIPTION(widget_activity_status, zmk_activity_state_changed);

/* ---------- init ---------- */

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    /* Child 0 — at widget_x=92..160. Renders to the user's BOTTOM 68 px. */
    lv_obj_t *hid_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(hid_canvas, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(hid_canvas, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    /* Child 1 — at widget_x=24..92. Renders to the user's MIDDLE 68 px. */
    lv_obj_t *middle_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(middle_canvas, LV_ALIGN_TOP_LEFT, 24, 0);
    lv_canvas_set_buffer(middle_canvas, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE,
                         LV_IMG_CF_TRUE_COLOR);

    /* Child 2 — at widget_x=-44..24, only the last 24 canvas columns visible.
     * Renders to the user's TOP ~24 px. */
    lv_obj_t *bat_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(bat_canvas, LV_ALIGN_TOP_LEFT, -44, 0);
    lv_canvas_set_buffer(bat_canvas, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);

    /* Default to active so the first render paints actual content — the
     * activity listener init below will overwrite this with the real state. */
    widget->state.active = true;

    /* Force a first render of every canvas with the widget's zero-initialised
     * state before the event listeners hook up. This guarantees every pixel
     * on the screen gets written at least once, refreshing the Sharp
     * memory-in-pixel LCD out of whatever the previous firmware wrote there. */
    draw_hid(widget->obj, widget->cbuf, &widget->state);
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
    draw_bat(widget->obj, widget->cbuf3, &widget->state);

    widget_battery_status_init();
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    widget_peripheral_battery_init();
#endif
    widget_output_status_init();
    widget_layer_status_init();
    widget_hid_indicators_status_init();
    widget_activity_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
