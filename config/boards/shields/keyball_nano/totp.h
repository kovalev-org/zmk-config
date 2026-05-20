/*
 * Internal interface for the Keyball39 TOTP store. Exposed to the &totp
 * behavior driver and the OLED label widget; not part of any public API.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 30 = floor(512 / SLOT_ENTRY_LEN) where SLOT_ENTRY_LEN = 1 + TOTP_LABEL_LEN = 17.
 * 512 is the Bluetooth ATT spec cap on a single attribute value, which limits
 * how much state the `slots` GATT characteristic can hold in one read. Bumping
 * past 30 requires a wire-format change (pagination or per-slot reads).
 */
#define TOTP_SLOT_COUNT     30
#define TOTP_LABEL_LEN      16
#define TOTP_CODE_LEN       6      /* digits */

/* User-facing OLED messages for the failure paths in `&totp <slot>`.
 * Centralised so the caller (behavior_totp.c) and the widget that renders
 * them agree on the exact text. */
#define TOTP_MSG_SLOT_EMPTY    "Slot empty"
#define TOTP_MSG_TIME_NOT_SET  "Time not set"

/*
 * Compute the current TOTP code for a slot.
 *
 * - Standard RFC 6238 (SHA-1, 6 digits, 30 s window).
 * - `code_out` must be at least TOTP_CODE_LEN+1 bytes; written zero-padded
 *   and NUL-terminated.
 * - Returns 0 on success.
 * - Returns -ENOENT if the slot is empty.
 * - Returns -EAGAIN if host time has not yet been pushed (no SET_TIME).
 */
int totp_generate_code(int slot, char *code_out);

/*
 * Copy the slot's label into `label_out` (always NUL-terminated, up to
 * TOTP_LABEL_LEN bytes of content). Returns true if the slot is occupied.
 */
bool totp_get_label(int slot, char label_out[TOTP_LABEL_LEN + 1]);

/*
 * Called by the behavior driver to make the OLED show `label` in place of
 * the layer name for ~3 s. Weak default does nothing, so the typing path
 * works even when the layer/TOTP widget isn't compiled in.
 */
void keyball39_totp_widget_flash_label(const char *label);
