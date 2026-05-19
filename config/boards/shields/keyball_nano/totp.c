/*
 * Keyball39 on-device TOTP store.
 *
 * Exposes a small custom GATT service on the right half (the BLE central):
 *   - command  (write, encrypted): TLV opcodes from the companion host.
 *   - slots    (read + notify, encrypted): packed view of slot occupancy +
 *                                          labels. Updated after every mutation.
 *
 * Slots and the host-supplied wall-clock offset are stored only in the right
 * half. Slot data persists in the same flash partition as BLE bonds via the
 * Zephyr settings subsystem; the time offset is RAM-only and re-pushed on every
 * connect by the companion tool.
 *
 * Wire protocol mirrors tools/totp-companion/src/protocol.rs. Keep both sides
 * in sync — there is no on-line version negotiation.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "totp.h"

LOG_MODULE_REGISTER(keyball39_totp, CONFIG_LOG_DEFAULT_LEVEL);

/* TOTP_SLOT_COUNT and TOTP_LABEL_LEN come from totp.h. */
#define TOTP_KEY_MAX_LEN    32
#define TOTP_SLOT_VERSION   1
#define TOTP_PERIOD_SEC     30

/* On-wire slots payload entry: 1 byte occupied flag + 16 byte label. */
#define SLOT_ENTRY_LEN      (1 + TOTP_LABEL_LEN)
#define SLOTS_PAYLOAD_LEN   (TOTP_SLOT_COUNT * SLOT_ENTRY_LEN)

enum totp_opcode {
    OP_SET_TIME     = 0x01,
    OP_SET_LABEL    = 0x02,
    OP_WRITE_SLOT   = 0x03,
    OP_DELETE_SLOT  = 0x04,
};

struct totp_slot {
    uint8_t version;                    /* = TOTP_SLOT_VERSION */
    uint8_t occupied;                   /* 0 or 1 */
    uint8_t key_len;                    /* 1..TOTP_KEY_MAX_LEN if occupied */
    uint8_t reserved;
    uint8_t label[TOTP_LABEL_LEN];      /* NUL-padded */
    uint8_t key[TOTP_KEY_MAX_LEN];      /* zero-padded */
} __packed;

BUILD_ASSERT(sizeof(struct totp_slot) == 4 + TOTP_LABEL_LEN + TOTP_KEY_MAX_LEN);

static struct totp_slot totp_slots[TOTP_SLOT_COUNT];

/*
 * Host time tracking. host_unix_offset_sec is set on every SET_TIME write:
 *      host_unix_offset_sec = host_time - k_uptime_seconds()
 * so current_unix_time() = host_unix_offset_sec + k_uptime_seconds().
 *
 * Not persisted — the companion tool re-pushes time on every connect.
 */
static int64_t host_unix_offset_sec;
static bool host_time_set;

/* GATT service UUIDs — keep in sync with protocol.rs. */
#define TOTP_SERVICE_UUID_VAL \
    BT_UUID_128_ENCODE(0xe0c25aab, 0x1d27, 0x4f1f, 0xb6a1, 0x71b011000001)
#define TOTP_COMMAND_UUID_VAL \
    BT_UUID_128_ENCODE(0xe0c25aab, 0x1d27, 0x4f1f, 0xb6a1, 0x71b011000002)
#define TOTP_SLOTS_UUID_VAL \
    BT_UUID_128_ENCODE(0xe0c25aab, 0x1d27, 0x4f1f, 0xb6a1, 0x71b011000003)

static const struct bt_uuid_128 totp_service_uuid =
    BT_UUID_INIT_128(TOTP_SERVICE_UUID_VAL);
static const struct bt_uuid_128 totp_command_uuid =
    BT_UUID_INIT_128(TOTP_COMMAND_UUID_VAL);
static const struct bt_uuid_128 totp_slots_uuid =
    BT_UUID_INIT_128(TOTP_SLOTS_UUID_VAL);

/* Forward decls. */
static void notify_slots_state(void);
static int  save_slot(int slot);
static int  delete_slot(int slot);
static void pack_slots(uint8_t out[SLOTS_PAYLOAD_LEN]);

/* ---------- Settings (persistent storage) ---------- */

#define SETTINGS_ROOT "kb39_totp"

static int totp_settings_set(const char *name, size_t len,
                              settings_read_cb read_cb, void *cb_arg)
{
    const char *next;
    int slot_idx;

    if (!settings_name_steq(name, "slot", &next) || next == NULL) {
        return -ENOENT;
    }
    slot_idx = atoi(next);
    if (slot_idx < 0 || slot_idx >= TOTP_SLOT_COUNT) {
        LOG_WRN("ignoring slot %d (out of range)", slot_idx);
        return 0;
    }
    if (len != sizeof(struct totp_slot)) {
        LOG_WRN("slot %d: size %u != expected %u (schema drift?), discarding",
                slot_idx, (unsigned)len, (unsigned)sizeof(struct totp_slot));
        return 0;
    }

    struct totp_slot buf;
    ssize_t r = read_cb(cb_arg, &buf, sizeof(buf));
    if (r < 0) {
        LOG_ERR("slot %d: read_cb failed: %d", slot_idx, (int)r);
        return r;
    }
    if (buf.version != TOTP_SLOT_VERSION) {
        LOG_WRN("slot %d: version %u != %u, discarding",
                slot_idx, buf.version, TOTP_SLOT_VERSION);
        return 0;
    }
    totp_slots[slot_idx] = buf;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(kb39_totp, SETTINGS_ROOT,
                                NULL, totp_settings_set, NULL, NULL);

static int save_slot(int slot)
{
    char path[32];
    snprintk(path, sizeof(path), SETTINGS_ROOT "/slot/%d", slot);
    int err = settings_save_one(path, &totp_slots[slot],
                                 sizeof(struct totp_slot));
    if (err) {
        LOG_ERR("settings_save_one(%s) failed: %d", path, err);
    }
    return err;
}

static int delete_slot(int slot)
{
    char path[32];
    snprintk(path, sizeof(path), SETTINGS_ROOT "/slot/%d", slot);
    memset(&totp_slots[slot], 0, sizeof(totp_slots[slot]));
    int err = settings_delete(path);
    if (err && err != -ENOENT) {
        LOG_ERR("settings_delete(%s) failed: %d", path, err);
    }
    return 0;
}

/* ---------- Slot packing for the read/notify characteristic ---------- */

static void pack_slots(uint8_t out[SLOTS_PAYLOAD_LEN])
{
    for (int i = 0; i < TOTP_SLOT_COUNT; i++) {
        uint8_t *entry = &out[i * SLOT_ENTRY_LEN];
        entry[0] = totp_slots[i].occupied ? 1 : 0;
        memcpy(&entry[1], totp_slots[i].label, TOTP_LABEL_LEN);
    }
}

/* ---------- GATT handlers ---------- */

static ssize_t slots_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    uint8_t packed[SLOTS_PAYLOAD_LEN];
    pack_slots(packed);
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                              packed, sizeof(packed));
}

static void slots_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    LOG_DBG("slots CCC = 0x%04x", value);
}

static int handle_set_time(const uint8_t *payload, size_t len)
{
    if (len != 8) {
        return -EINVAL;
    }
    int64_t host_secs = (int64_t)sys_get_le64(payload);
    int64_t uptime_secs = k_uptime_get() / 1000;
    host_unix_offset_sec = host_secs - uptime_secs;
    host_time_set = true;
    LOG_INF("time synced: unix=%lld offset=%lld",
             (long long)host_secs, (long long)host_unix_offset_sec);
    return 0;
}

static int handle_set_label(const uint8_t *payload, size_t len)
{
    if (len != 1 + TOTP_LABEL_LEN) {
        return -EINVAL;
    }
    uint8_t slot = payload[0];
    if (slot >= TOTP_SLOT_COUNT) {
        return -EINVAL;
    }
    if (!totp_slots[slot].occupied) {
        return -ENOENT;
    }
    memcpy(totp_slots[slot].label, payload + 1, TOTP_LABEL_LEN);
    return save_slot(slot);
}

static int handle_write_slot(const uint8_t *payload, size_t len)
{
    /* slot(1) + label(16) + key_len(1) + key(>=1) */
    if (len < 1 + TOTP_LABEL_LEN + 1 + 1) {
        return -EINVAL;
    }
    uint8_t slot = payload[0];
    uint8_t key_len = payload[1 + TOTP_LABEL_LEN];
    if (slot >= TOTP_SLOT_COUNT) {
        return -EINVAL;
    }
    if (key_len == 0 || key_len > TOTP_KEY_MAX_LEN) {
        return -EINVAL;
    }
    if (len != 1 + TOTP_LABEL_LEN + 1 + key_len) {
        return -EINVAL;
    }

    struct totp_slot *s = &totp_slots[slot];
    s->version = TOTP_SLOT_VERSION;
    s->occupied = 1;
    s->key_len = key_len;
    s->reserved = 0;
    memcpy(s->label, payload + 1, TOTP_LABEL_LEN);
    memset(s->key, 0, sizeof(s->key));
    memcpy(s->key, payload + 1 + TOTP_LABEL_LEN + 1, key_len);
    return save_slot(slot);
}

static int handle_delete_slot(const uint8_t *payload, size_t len)
{
    if (len != 1) {
        return -EINVAL;
    }
    uint8_t slot = payload[0];
    if (slot >= TOTP_SLOT_COUNT) {
        return -EINVAL;
    }
    return delete_slot(slot);
}

static ssize_t command_write(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len,
                              uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *data = buf;
    uint8_t op = data[0];
    const uint8_t *payload = data + 1;
    size_t plen = len - 1;

    int err;
    bool slots_changed = false;

    switch (op) {
    case OP_SET_TIME:
        err = handle_set_time(payload, plen);
        break;
    case OP_SET_LABEL:
        err = handle_set_label(payload, plen);
        slots_changed = (err == 0);
        break;
    case OP_WRITE_SLOT:
        err = handle_write_slot(payload, plen);
        slots_changed = (err == 0);
        break;
    case OP_DELETE_SLOT:
        err = handle_delete_slot(payload, plen);
        slots_changed = (err == 0);
        break;
    default:
        LOG_WRN("unknown opcode 0x%02x", op);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (err < 0) {
        LOG_WRN("opcode 0x%02x failed: %d", op, err);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (slots_changed) {
        notify_slots_state();
    }
    return len;
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(totp_svc,
    BT_GATT_PRIMARY_SERVICE(&totp_service_uuid),
    BT_GATT_CHARACTERISTIC(&totp_command_uuid.uuid,
        BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE_ENCRYPT,
        NULL, command_write, NULL),
    BT_GATT_CHARACTERISTIC(&totp_slots_uuid.uuid,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ_ENCRYPT,
        slots_read, NULL, NULL),
    BT_GATT_CCC(slots_ccc_changed,
        BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
);
/* clang-format on */

/* attrs layout:
 *   [0] primary service
 *   [1] command char declaration
 *   [2] command char value
 *   [3] slots char declaration
 *   [4] slots char value          <- notify on this
 *   [5] CCC
 */
#define SLOTS_VALUE_ATTR (&totp_svc.attrs[4])

static void notify_slots_state(void)
{
    uint8_t packed[SLOTS_PAYLOAD_LEN];
    pack_slots(packed);
    int err = bt_gatt_notify(NULL, SLOTS_VALUE_ATTR, packed, sizeof(packed));
    if (err && err != -ENOTCONN) {
        LOG_WRN("bt_gatt_notify failed: %d", err);
    }
}

/* ---------- SHA-1 (RFC 3174) ---------- */

#define SHA1_BLOCK_LEN  64
#define SHA1_DIGEST_LEN 20

struct sha1_ctx {
    uint32_t state[5];
    uint64_t count_bits;
    uint8_t  buf[SHA1_BLOCK_LEN];
    size_t   buf_used;
};

static inline uint32_t rol32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1_transform(uint32_t state[5], const uint8_t block[SHA1_BLOCK_LEN])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16)
             | ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_init(struct sha1_ctx *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count_bits = 0;
    ctx->buf_used = 0;
}

static void sha1_update(struct sha1_ctx *ctx, const uint8_t *data, size_t len)
{
    ctx->count_bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = SHA1_BLOCK_LEN - ctx->buf_used;
        if (take > len) take = len;
        memcpy(&ctx->buf[ctx->buf_used], data, take);
        ctx->buf_used += take;
        data += take;
        len -= take;
        if (ctx->buf_used == SHA1_BLOCK_LEN) {
            sha1_transform(ctx->state, ctx->buf);
            ctx->buf_used = 0;
        }
    }
}

static void sha1_final(struct sha1_ctx *ctx, uint8_t out[SHA1_DIGEST_LEN])
{
    uint64_t bits = ctx->count_bits;
    /* Append 0x80 then zero-pad to leave 8 bytes for length. */
    ctx->buf[ctx->buf_used++] = 0x80;
    if (ctx->buf_used > SHA1_BLOCK_LEN - 8) {
        memset(&ctx->buf[ctx->buf_used], 0, SHA1_BLOCK_LEN - ctx->buf_used);
        sha1_transform(ctx->state, ctx->buf);
        ctx->buf_used = 0;
    }
    memset(&ctx->buf[ctx->buf_used], 0, SHA1_BLOCK_LEN - 8 - ctx->buf_used);
    for (int i = 0; i < 8; i++) {
        ctx->buf[SHA1_BLOCK_LEN - 1 - i] = (uint8_t)(bits >> (i * 8));
    }
    sha1_transform(ctx->state, ctx->buf);
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/* ---------- HMAC-SHA1 (RFC 2104) ---------- */

static void hmac_sha1(const uint8_t *key, size_t key_len,
                       const uint8_t *msg, size_t msg_len,
                       uint8_t out[SHA1_DIGEST_LEN])
{
    uint8_t k_pad[SHA1_BLOCK_LEN];
    uint8_t k_hashed[SHA1_DIGEST_LEN];
    struct sha1_ctx ctx;

    if (key_len > SHA1_BLOCK_LEN) {
        sha1_init(&ctx);
        sha1_update(&ctx, key, key_len);
        sha1_final(&ctx, k_hashed);
        key = k_hashed;
        key_len = SHA1_DIGEST_LEN;
    }

    /* inner */
    memset(k_pad, 0x36, SHA1_BLOCK_LEN);
    for (size_t i = 0; i < key_len; i++) k_pad[i] ^= key[i];
    sha1_init(&ctx);
    sha1_update(&ctx, k_pad, SHA1_BLOCK_LEN);
    sha1_update(&ctx, msg, msg_len);
    uint8_t inner[SHA1_DIGEST_LEN];
    sha1_final(&ctx, inner);

    /* outer */
    memset(k_pad, 0x5c, SHA1_BLOCK_LEN);
    for (size_t i = 0; i < key_len; i++) k_pad[i] ^= key[i];
    sha1_init(&ctx);
    sha1_update(&ctx, k_pad, SHA1_BLOCK_LEN);
    sha1_update(&ctx, inner, SHA1_DIGEST_LEN);
    sha1_final(&ctx, out);
}

/* ---------- TOTP (RFC 6238) ---------- */

int totp_generate_code(int slot, char *code_out)
{
    if (slot < 0 || slot >= TOTP_SLOT_COUNT) {
        return -EINVAL;
    }
    if (!totp_slots[slot].occupied) {
        return -ENOENT;
    }
    if (!host_time_set) {
        return -EAGAIN;
    }

    int64_t now_sec = host_unix_offset_sec + k_uptime_get() / 1000;
    if (now_sec < 0) {
        return -EAGAIN;
    }
    uint64_t t = (uint64_t)now_sec / TOTP_PERIOD_SEC;
    uint8_t t_be[8];
    for (int i = 0; i < 8; i++) {
        t_be[7 - i] = (uint8_t)(t >> (i * 8));
    }

    uint8_t hmac[SHA1_DIGEST_LEN];
    hmac_sha1(totp_slots[slot].key, totp_slots[slot].key_len,
              t_be, sizeof(t_be), hmac);

    int offset = hmac[SHA1_DIGEST_LEN - 1] & 0x0F;
    uint32_t binary = ((uint32_t)(hmac[offset] & 0x7F) << 24)
                    | ((uint32_t)hmac[offset + 1] << 16)
                    | ((uint32_t)hmac[offset + 2] << 8)
                    | (uint32_t)hmac[offset + 3];
    uint32_t code = binary % 1000000u;

    snprintf(code_out, TOTP_CODE_LEN + 1, "%06u", code);
    return 0;
}

bool totp_get_label(int slot, char label_out[TOTP_LABEL_LEN + 1])
{
    if (slot < 0 || slot >= TOTP_SLOT_COUNT) return false;
    if (!totp_slots[slot].occupied) return false;
    memcpy(label_out, totp_slots[slot].label, TOTP_LABEL_LEN);
    label_out[TOTP_LABEL_LEN] = '\0';
    return true;
}

/* Weak default — overridden by the widget when CONFIG_KEYBALL39_TOTP_WIDGET=y. */
__weak void keyball39_totp_widget_flash_label(const char *label)
{
    ARG_UNUSED(label);
}
