# keyball39-totp

A small Rust CLI for provisioning TOTP slots on a Keyball39 keyboard over BLE.

The keyboard stores up to 16 TOTP slots (key + 16-byte label) in its settings
partition. This tool writes them, renames them, deletes them, and lists what's
where. Keys are **write-only** — they can be written but never read back.

The matching keyboard-side behavior `&totp <slot>` (firmware) takes one slot
index, computes the current 6-digit TOTP, types it via HID, and flashes the
slot's label on the OLED for ~3 seconds in place of the layer name. If the
slot is empty the OLED shows `Slot empty`; if host time hasn't been synced
since boot it shows `Time not set` and refuses to type. On the stock
Keyball39 keymap `&totp` is bound to a dedicated `TOTP` layer (hold `LH4`,
the key right of the Sys activator) with slots 1–12 mapped to the same
physical positions Fn uses for F1–F12.

## What you need

- A Keyball39 with firmware that exposes the TOTP GATT service (`e0c25aab-…`).
- The keyboard already paired to this host through the OS Bluetooth settings —
  the tool does not initiate pairing.
- A working BLE adapter that bluest supports (Windows 10+, macOS, Linux with
  BlueZ).
- Rust 1.74+ (only to build; the release binary is standalone).

## Build

```sh
cd tools/totp-companion
cargo build --release
# binary at target/release/keyball39-totp[.exe]
```

The release binary has no runtime dependencies; copy it anywhere on `PATH`.

## Usage

Every command implicitly pushes the current host time to the keyboard before
doing anything else, so the keyboard's clock is re-synced on each invocation.

```sh
# List the current state of all 16 slots
keyball39-totp list

# Provision a new key (slot 0, label "github", secret as base32)
keyball39-totp write 0 github JBSWY3DPEHPK3PXP

# Rename slot 0 without touching the key
keyball39-totp set-label 0 github-work

# Erase slot 0
keyball39-totp delete 0

# Skip the confirmation prompt on overwrite / delete
keyball39-totp write 0 github JBSWY3DPEHPK3PXP --force
keyball39-totp delete 0 --force

# Target a keyboard with a non-default advertised name
keyball39-totp --name "Keyball39 (work)" list
```

The base32 secret is the same string you'd paste into an authenticator app from
a service's manual 2FA setup screen. Whitespace and dashes are stripped; case is
ignored.

### Output

`list` and every mutating command end by printing the full slot table:

```
slot  label
----  -----
   0  github-work
   1  aws-root
   2  -
   3  -
   …  …
```

`-` means empty. The keyboard is the source of truth; the tool never caches.

## Constraints

| Thing | Limit | Why |
|---|---|---|
| Slots | 16 | Compile-time constant on the firmware side |
| Label | 16 bytes UTF-8, no NUL | Fits one line on the OLED in UNSCII-8 |
| Key | 1..32 bytes (after base32 decode) | Covers SHA-1 and SHA-256 HMAC keys |
| TOTP variant | SHA-1, 6 digits, 30s period | v1 firmware only generates this |

A label longer than 16 bytes, or a secret that doesn't decode as base32, gets
rejected client-side before anything goes over the air.

## What's stored where

| Data | Lives on | Readable by tool | Survives reflash |
|---|---|---|---|
| TOTP keys | Keyboard settings partition | No (write-only) | Yes |
| Labels | Keyboard settings partition | Yes | Yes |
| Host time offset | Keyboard RAM | n/a | No — re-pushed on every connect |

The settings partition is the same flash region that holds your BLE bonds, so a
UF2 reflash preserves slots. Building with the `settings_reset` shield wipes
both bonds and slots.

**Important:** time is RAM-only on the keyboard. After any power cycle or
reflash you must run *any* command in this tool (even `list`) before pressing
`&totp` — the keyboard's TOTP behavior refuses to type until host time has
been synced at least once.

## How discovery works

The tool asks the OS for the list of *connected paired* BLE devices first,
then falls back to OS-known-but-not-currently-connected devices that expose
the TOTP service, and only as a last resort starts an advertisement scan.

This matters on Windows specifically: when the keyboard is paired and actively
typing, it has a live HID connection and isn't advertising at all — pure
scan-based discovery can't find it. Querying OS-known devices via WinRT works
regardless of advertisement state.

If discovery times out, check that:

1. The right half of the keyboard is on and within range.
2. It's paired to *this* host (not a different host in the BLE profile rotation).
3. The advertised name matches `--name` (default `Keyball39`).

## Protocol (for hackers)

Custom GATT service, two characteristics:

| UUID | Direction | Properties |
|---|---|---|
| `e0c25aab-1d27-4f1f-b6a1-71b011000001` | service | — |
| `…-71b011000002` (command) | host → keyboard | Write, encrypted |
| `…-71b011000003` (slots) | keyboard → host | Read + Notify, encrypted |

Commands on the command characteristic are TLV: one opcode byte, then payload.

| Opcode | Name | Payload |
|---|---|---|
| `0x01` | `SET_TIME` | `u64 LE` Unix seconds |
| `0x02` | `SET_LABEL` | `u8 slot, [16]u8 label` |
| `0x03` | `WRITE_SLOT` | `u8 slot, [16]u8 label, u8 key_len, key bytes` |
| `0x04` | `DELETE_SLOT` | `u8 slot` |

Labels are NUL-padded to 16 bytes. Keys are 1..32 raw bytes (no length prefix
on the wire beyond `key_len`).

The slots characteristic is 16 × 17 bytes = 272 bytes:

```
struct slot_entry {
    u8 occupied;          // 0 = empty, 1 = occupied
    u8 label[16];         // NUL-padded
};                        // 17 bytes
```

After every mutating command the tool does a fresh read of the slots
characteristic (write-then-read). The firmware *also* fires a notification on
the slots characteristic after each mutation, but the payload (272 bytes) is
larger than the default ATT notify limit (~20 bytes), so the host currently
ignores notifications and relies on the read path — ATT_READ_BLOB_REQ handles
the long value transparently. A future MTU bump on the firmware side would
let the notify path work, but it's not needed for v1.

`SET_TIME` doesn't change slot state, so the tool doesn't follow it with a
read — it relies on the GATT-level write-with-response ack.

Both characteristics require an encrypted link. Pair through the OS first.

## Security caveats

- The flash partition where keys live is **not encrypted**. Anyone with
  physical access to the keyboard and a JTAG/SWD probe can dump the secrets.
  Same threat model as YubiKey OATH without a PIN.
- Anyone who pairs to your keyboard can write to it. Don't pair to hosts you
  don't trust.
- The tool intentionally cannot read keys back. Back up your secrets in a
  password manager when you first scan the QR code — there's no recovery path
  from the keyboard.

## Project layout

```
src/
├── main.rs        # clap dispatch
├── ble.rs         # btleplug client (scan, connect, write, read, notify)
├── commands.rs    # list / write / set-label / delete
└── protocol.rs    # opcodes, encoders, slot decoder, UUIDs
```

`protocol.rs` is the single source of truth for the wire format; the keyboard
firmware mirrors the same constants.

A future TUI or GUI frontend can reuse `ble.rs` and `commands.rs` unchanged
— `main.rs` is the only CLI-specific code.
