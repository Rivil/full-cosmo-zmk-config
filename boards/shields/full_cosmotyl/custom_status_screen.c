/*
 * SPDX-License-Identifier: MIT
 *
 * Entry point called by ZMK's display subsystem when
 * CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y. Returns the root lv_obj that
 * holds the custom status widget. Only linked for the left (central) build —
 * see CMakeLists.txt — so there is no collision with nice_view's own
 * zmk_display_status_screen() (which is disabled via
 * CONFIG_NICE_VIEW_WIDGET_STATUS=n).
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "widgets/status.h"

static struct zmk_widget_status status_widget;

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    zmk_widget_status_init(&status_widget, screen);
    lv_obj_align(zmk_widget_status_obj(&status_widget), LV_ALIGN_TOP_LEFT, 0, 0);
    return screen;
}
