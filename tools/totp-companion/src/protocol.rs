//! Wire protocol shared with the Keyball39 firmware.
//!
//! UUIDs are random v4 values picked once. Keep this file in sync with the
//! corresponding C side under `config/boards/shields/keyball_nano/`.

use anyhow::{anyhow, bail, Result};
use uuid::{uuid, Uuid};

pub const SERVICE_UUID: Uuid = uuid!("e0c25aab-1d27-4f1f-b6a1-71b011000001");
pub const COMMAND_CHAR_UUID: Uuid = uuid!("e0c25aab-1d27-4f1f-b6a1-71b011000002");
pub const SLOTS_CHAR_UUID: Uuid = uuid!("e0c25aab-1d27-4f1f-b6a1-71b011000003");

// 30 = floor(512 / SLOT_ENTRY_LEN). 512 is the BT ATT max attribute value
// length; raising SLOT_COUNT past 30 requires changing the slots GATT
// characteristic layout (e.g. pagination), not just bumping this constant.
pub const SLOT_COUNT: usize = 30;
pub const LABEL_LEN: usize = 16;
pub const KEY_MAX_LEN: usize = 64;
pub const SLOT_ENTRY_LEN: usize = 1 + LABEL_LEN;
pub const SLOTS_PAYLOAD_LEN: usize = SLOT_COUNT * SLOT_ENTRY_LEN;

#[repr(u8)]
#[derive(Copy, Clone, Debug)]
pub enum Opcode {
    SetTime = 0x01,
    SetLabel = 0x02,
    WriteSlot = 0x03,
    DeleteSlot = 0x04,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Slot {
    pub label: String,
}

pub fn encode_set_time(unix_seconds: u64) -> Vec<u8> {
    let mut buf = Vec::with_capacity(1 + 8);
    buf.push(Opcode::SetTime as u8);
    buf.extend_from_slice(&unix_seconds.to_le_bytes());
    buf
}

pub fn encode_set_label(slot: u8, label: &str) -> Result<Vec<u8>> {
    check_slot(slot)?;
    let label_bytes = pack_label(label)?;
    let mut buf = Vec::with_capacity(1 + 1 + LABEL_LEN);
    buf.push(Opcode::SetLabel as u8);
    buf.push(slot);
    buf.extend_from_slice(&label_bytes);
    Ok(buf)
}

pub fn encode_write_slot(slot: u8, label: &str, key: &[u8]) -> Result<Vec<u8>> {
    check_slot(slot)?;
    if key.is_empty() || key.len() > KEY_MAX_LEN {
        bail!(
            "key length {} out of range (1..={})",
            key.len(),
            KEY_MAX_LEN
        );
    }
    let label_bytes = pack_label(label)?;
    let mut buf = Vec::with_capacity(1 + 1 + LABEL_LEN + 1 + key.len());
    buf.push(Opcode::WriteSlot as u8);
    buf.push(slot);
    buf.extend_from_slice(&label_bytes);
    buf.push(key.len() as u8);
    buf.extend_from_slice(key);
    Ok(buf)
}

pub fn encode_delete_slot(slot: u8) -> Result<Vec<u8>> {
    check_slot(slot)?;
    Ok(vec![Opcode::DeleteSlot as u8, slot])
}

pub fn decode_slots(payload: &[u8]) -> Result<[Option<Slot>; SLOT_COUNT]> {
    if payload.len() != SLOTS_PAYLOAD_LEN {
        bail!(
            "slots payload length {} != expected {}",
            payload.len(),
            SLOTS_PAYLOAD_LEN
        );
    }
    let mut out: [Option<Slot>; SLOT_COUNT] = std::array::from_fn(|_| None);
    for (i, slot) in out.iter_mut().enumerate() {
        let off = i * SLOT_ENTRY_LEN;
        let occupied = payload[off] != 0;
        if !occupied {
            continue;
        }
        let label_bytes = &payload[off + 1..off + 1 + LABEL_LEN];
        let end = label_bytes
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(LABEL_LEN);
        let label = std::str::from_utf8(&label_bytes[..end])
            .map_err(|e| anyhow!("slot {} label is not UTF-8: {}", i, e))?
            .to_owned();
        *slot = Some(Slot { label });
    }
    Ok(out)
}

fn check_slot(slot: u8) -> Result<()> {
    if (slot as usize) >= SLOT_COUNT {
        bail!("slot {} out of range (0..{})", slot, SLOT_COUNT);
    }
    Ok(())
}

fn pack_label(label: &str) -> Result<[u8; LABEL_LEN]> {
    let bytes = label.as_bytes();
    if bytes.len() > LABEL_LEN {
        bail!(
            "label too long: {} bytes (max {})",
            bytes.len(),
            LABEL_LEN
        );
    }
    if bytes.contains(&0) {
        bail!("label must not contain NUL bytes");
    }
    let mut out = [0u8; LABEL_LEN];
    out[..bytes.len()].copy_from_slice(bytes);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn set_time_layout() {
        let buf = encode_set_time(0x0102030405060708);
        assert_eq!(buf, vec![0x01, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01]);
    }

    #[test]
    fn write_slot_layout() {
        let buf = encode_write_slot(3, "github", &[0xaa; 20]).unwrap();
        assert_eq!(buf[0], 0x03);
        assert_eq!(buf[1], 3);
        assert_eq!(&buf[2..8], b"github");
        assert_eq!(&buf[8..18], &[0u8; 10]);
        assert_eq!(buf[18], 20);
        assert_eq!(&buf[19..], &[0xaa; 20]);
    }

    #[test]
    fn label_too_long_rejected() {
        assert!(encode_set_label(0, "0123456789abcdefg").is_err());
    }

    #[test]
    fn label_with_nul_rejected() {
        assert!(encode_set_label(0, "abc\0def").is_err());
    }

    #[test]
    fn slot_out_of_range_rejected() {
        assert!(encode_delete_slot(SLOT_COUNT as u8).is_err());
    }

    #[test]
    fn decode_round_trip() {
        let mut buf = vec![0u8; SLOTS_PAYLOAD_LEN];
        buf[0] = 1;
        buf[1..7].copy_from_slice(b"github");
        let off = 3 * SLOT_ENTRY_LEN;
        buf[off] = 1;
        buf[off + 1..off + 4].copy_from_slice(b"aws");
        let slots = decode_slots(&buf).unwrap();
        assert_eq!(slots[0].as_ref().unwrap().label, "github");
        assert!(slots[1].is_none());
        assert_eq!(slots[3].as_ref().unwrap().label, "aws");
    }
}
