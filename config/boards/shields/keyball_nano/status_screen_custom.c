/*
 * Custom status screen. Mirrors upstream's `app/src/display/status_screen.c`
 * layout but swaps the layer-status widget for our layer-or-TOTP variant
 * (see widget_layer_or_totp.{c,h}) so the OLED can show TOTP-related text in
 * the layer-name slot when &totp fires.
 *
 * Selected via CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y in keyball39_right.conf,
 * which also re-enables the per-widget Kconfigs and the LVGL memory pool size
 * that BUILT_IN would have implied — see that file's comment.
 */

#include <zmk/display/widgets/output_status.h>
#include <zmk/display/widgets/peripheral_status.h>
#include <zmk/display/widgets/battery_status.h>
#include <zmk/display/widgets/wpm_status.h>
#include <zmk/display/status_screen.h>

#include "widget_layer_or_totp.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
static struct zmk_widget_battery_status battery_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
static struct zmk_widget_output_status output_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
static struct zmk_widget_peripheral_status peripheral_status_widget;
#endif

static struct zmk_widget_layer_or_totp layer_or_totp_widget;

#if IS_ENABLED(CONFIG_ZMK_WIDGET_WPM_STATUS)
static struct zmk_widget_wpm_status wpm_status_widget;
#endif

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen;
    screen = lv_obj_create(NULL);

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
    zmk_widget_battery_status_init(&battery_status_widget, screen);
    lv_obj_align(zmk_widget_battery_status_obj(&battery_status_widget), LV_ALIGN_TOP_RIGHT, 0, 0);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
    zmk_widget_output_status_init(&output_status_widget, screen);
    lv_obj_align(zmk_widget_output_status_obj(&output_status_widget), LV_ALIGN_TOP_LEFT, 0, 0);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
    zmk_widget_peripheral_status_init(&peripheral_status_widget, screen);
    lv_obj_align(zmk_widget_peripheral_status_obj(&peripheral_status_widget), LV_ALIGN_TOP_LEFT, 0,
                 0);
#endif

    zmk_widget_layer_or_totp_init(&layer_or_totp_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_layer_or_totp_obj(&layer_or_totp_widget),
                                lv_theme_get_font_small(screen), LV_PART_MAIN);
    lv_obj_align(zmk_widget_layer_or_totp_obj(&layer_or_totp_widget),
                 LV_ALIGN_BOTTOM_LEFT, 0, 0);

#if IS_ENABLED(CONFIG_ZMK_WIDGET_WPM_STATUS)
    zmk_widget_wpm_status_init(&wpm_status_widget, screen);
    lv_obj_align(zmk_widget_wpm_status_obj(&wpm_status_widget), LV_ALIGN_BOTTOM_RIGHT, 0, 0);
#endif
    return screen;
}
