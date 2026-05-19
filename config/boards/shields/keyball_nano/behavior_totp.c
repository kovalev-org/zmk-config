/*
 * &totp <slot> — generate a TOTP code for the given slot and type it out as
 * keystrokes on the host, plus flash the slot's label on the OLED for ~3 s.
 *
 * Slot data is held by totp.c (settings-backed). This file is just the ZMK
 * behavior glue + the digit-typing state machine.
 */

#define DT_DRV_COMPAT zmk_behavior_totp

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#include "totp.h"

LOG_MODULE_DECLARE(keyball39_totp, CONFIG_LOG_DEFAULT_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* HID usage IDs for digit keys (top-row, not numpad). */
static const uint8_t digit_usage_ids[10] = {
    HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS,
    HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION,
    HID_USAGE_KEY_KEYBOARD_2_AND_AT,
    HID_USAGE_KEY_KEYBOARD_3_AND_HASH,
    HID_USAGE_KEY_KEYBOARD_4_AND_DOLLAR,
    HID_USAGE_KEY_KEYBOARD_5_AND_PERCENT,
    HID_USAGE_KEY_KEYBOARD_6_AND_CARET,
    HID_USAGE_KEY_KEYBOARD_7_AND_AMPERSAND,
    HID_USAGE_KEY_KEYBOARD_8_AND_ASTERISK,
    HID_USAGE_KEY_KEYBOARD_9_AND_LEFT_PARENTHESIS,
};

/* Typing state machine — single global, since we type one code at a time. */
#define INTER_KEY_MS 5

struct typing_state {
    struct k_work_delayable work;
    char code[TOTP_CODE_LEN + 1];
    uint8_t index;          /* 0..TOTP_CODE_LEN */
    bool pressing;          /* true: next action is press; false: release */
    bool active;
};

static struct typing_state typing;

static void send_digit_event(char digit, bool press)
{
    int d = digit - '0';
    if (d < 0 || d > 9) {
        return;
    }
    uint32_t encoded = ZMK_HID_USAGE(HID_USAGE_KEY, digit_usage_ids[d]);
    raise_zmk_keycode_state_changed_from_encoded(encoded, press, k_uptime_get());
}

static void typing_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!typing.active) {
        return;
    }
    if (typing.index >= TOTP_CODE_LEN) {
        typing.active = false;
        return;
    }

    send_digit_event(typing.code[typing.index], typing.pressing);

    if (typing.pressing) {
        typing.pressing = false;
    } else {
        typing.pressing = true;
        typing.index++;
    }

    if (typing.index < TOTP_CODE_LEN || typing.pressing == false) {
        k_work_schedule(&typing.work, K_MSEC(INTER_KEY_MS));
    } else {
        typing.active = false;
    }
}

static int totp_init_once(void)
{
    static bool inited;
    if (inited) {
        return 0;
    }
    k_work_init_delayable(&typing.work, typing_work_cb);
    inited = true;
    return 0;
}

static int on_totp_pressed(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(event);
    totp_init_once();

    int slot = (int)binding->param1;

    if (typing.active) {
        LOG_WRN("totp: still typing previous code, ignoring");
        return ZMK_BEHAVIOR_OPAQUE;
    }

    int err = totp_generate_code(slot, typing.code);
    if (err == -ENOENT) {
        LOG_WRN("totp: slot %d is empty", slot);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (err == -EAGAIN) {
        LOG_WRN("totp: host time not yet synced; run `keyball39-totp list` to push it");
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (err) {
        LOG_WRN("totp: slot %d: error %d", slot, err);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    char label[TOTP_LABEL_LEN + 1] = {0};
    if (totp_get_label(slot, label)) {
        keyball39_totp_widget_flash_label(label);
    }

    typing.index = 0;
    typing.pressing = true;
    typing.active = true;
    k_work_schedule(&typing.work, K_NO_WAIT);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_totp_released(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_totp_driver_api = {
    .binding_pressed = on_totp_pressed,
    .binding_released = on_totp_released,
};

#define KP_INST(n)                                                                                 \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                    \
                            &behavior_totp_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
