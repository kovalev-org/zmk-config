#pragma once

#include <lvgl.h>
#include <zephyr/sys/slist.h>

struct zmk_widget_layer_or_totp {
    lv_obj_t *obj;
    sys_snode_t node;
};

int zmk_widget_layer_or_totp_init(struct zmk_widget_layer_or_totp *widget,
                                    lv_obj_t *parent);

lv_obj_t *zmk_widget_layer_or_totp_obj(struct zmk_widget_layer_or_totp *widget);
