/*
 * Drop-in replacement for ZMK's upstream layer-status widget.
 *
 * Renders the current layer name; when the &totp behavior fires it swaps the
 * text to the slot's label for ~3 s and then reverts. Implements the weak
 * `keyball39_totp_widget_flash_label` declared in totp.h.
 *
 * All LVGL access is funneled through the display work queue
 * (`zmk_display_work_q`) so LVGL is only ever touched from one thread.
 */

#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include "totp.h"
#include "widget_layer_or_totp.h"

LOG_MODULE_DECLARE(keyball39_totp, CONFIG_LOG_DEFAULT_LEVEL);

#define TOTP_LABEL_HOLD_MS 3000

enum widget_mode {
    MODE_LAYER,
    MODE_TOTP,
};

struct layer_state {
    zmk_keymap_layer_index_t index;
    const char *name;
};

/* Shared between the event-manager thread and the display thread; protected
 * by `state_mutex`. */
static K_MUTEX_DEFINE(state_mutex);
static struct layer_state shared_layer = {0};
static char shared_totp_label[TOTP_LABEL_LEN + 1] = {0};
static enum widget_mode shared_mode = MODE_LAYER;

/* List of widget instances — only read from the display thread. */
static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static void render_all(void)
{
    enum widget_mode mode;
    char text[32];

    k_mutex_lock(&state_mutex, K_FOREVER);
    mode = shared_mode;
    if (mode == MODE_TOTP) {
        snprintf(text, sizeof(text), LV_SYMBOL_KEYBOARD " %s", shared_totp_label);
    } else if (shared_layer.name && shared_layer.name[0]) {
        snprintf(text, sizeof(text), LV_SYMBOL_KEYBOARD " %s", shared_layer.name);
    } else {
        snprintf(text, sizeof(text), LV_SYMBOL_KEYBOARD " %u",
                 (unsigned)shared_layer.index);
    }
    k_mutex_unlock(&state_mutex);

    struct zmk_widget_layer_or_totp *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        lv_label_set_text(w->obj, text);
    }
}

/* ----- layer-state-changed event handling (event-mgr → display thread) ----- */

static struct layer_state get_layer_state(const zmk_event_t *eh)
{
    ARG_UNUSED(eh);
    zmk_keymap_layer_index_t i = zmk_keymap_highest_layer_active();
    return (struct layer_state){
        .index = i,
        .name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(i)),
    };
}

static void layer_state_update_cb(struct layer_state new_state)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    shared_layer = new_state;
    enum widget_mode mode = shared_mode;
    k_mutex_unlock(&state_mutex);
    if (mode == MODE_LAYER) {
        render_all();
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_or_totp_layer,
                             struct layer_state,
                             layer_state_update_cb,
                             get_layer_state)
ZMK_SUBSCRIPTION(widget_layer_or_totp_layer, zmk_layer_state_changed);

/* ----- TOTP label flash (behavior thread → display thread) ----- */

static void render_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    render_all();
}

static void revert_work_cb(struct k_work *work);

static K_WORK_DEFINE(render_work, render_work_cb);
static K_WORK_DELAYABLE_DEFINE(revert_work, revert_work_cb);

static void revert_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    k_mutex_lock(&state_mutex, K_FOREVER);
    shared_mode = MODE_LAYER;
    k_mutex_unlock(&state_mutex);
    render_all();
}

void keyball39_totp_widget_flash_label(const char *label)
{
    if (label == NULL || !zmk_display_is_initialized()) {
        return;
    }
    k_mutex_lock(&state_mutex, K_FOREVER);
    strncpy(shared_totp_label, label, TOTP_LABEL_LEN);
    shared_totp_label[TOTP_LABEL_LEN] = '\0';
    shared_mode = MODE_TOTP;
    k_mutex_unlock(&state_mutex);

    k_work_submit_to_queue(zmk_display_work_q(), &render_work);
    k_work_reschedule_for_queue(zmk_display_work_q(), &revert_work,
                                 K_MSEC(TOTP_LABEL_HOLD_MS));
}

/* ----- widget lifecycle ----- */

int zmk_widget_layer_or_totp_init(struct zmk_widget_layer_or_totp *widget,
                                    lv_obj_t *parent)
{
    widget->obj = lv_label_create(parent);
    sys_slist_append(&widgets, &widget->node);
    widget_layer_or_totp_layer_init();
    return 0;
}

lv_obj_t *zmk_widget_layer_or_totp_obj(struct zmk_widget_layer_or_totp *widget)
{
    return widget->obj;
}
