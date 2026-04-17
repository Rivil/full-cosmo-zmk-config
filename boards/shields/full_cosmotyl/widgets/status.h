/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>
#include "util.h"

struct zmk_widget_status {
    sys_snode_t node;
    lv_obj_t *obj;
    /* Three 68x68 canvases laid out side-by-side in the unrotated widget;
     * after the per-canvas 90° rotate they become the top, middle and bottom
     * thirds of the vertical display. */
    lv_color_t cbuf[CANVAS_SIZE * CANVAS_SIZE];
    lv_color_t cbuf2[CANVAS_SIZE * CANVAS_SIZE];
    lv_color_t cbuf3[CANVAS_SIZE * CANVAS_SIZE];
    struct status_state state;
};

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget);
