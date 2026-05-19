/*
 * Internal interface for the Keyball39 TOTP store. Exposed to the &totp
 * behavior driver and the OLED label widget; not part of any public API.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TOTP_SLOT_COUNT     16
#define TOTP_LABEL_LEN      16
#define TOTP_CODE_LEN       6      /* digits */

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
