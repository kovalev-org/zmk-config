//! Command implementations. Each function takes a connected [`Keyboard`] and
//! returns a result; the CLI layer in `main.rs` just parses args and calls
//! these. A future TUI/GUI frontend can call them the same way.

use anyhow::{anyhow, bail, Result};
use dialoguer::Confirm;

use crate::ble::Keyboard;
use crate::protocol::{self, Slot, SLOT_COUNT};

pub async fn list(kb: &Keyboard) -> Result<()> {
    let slots = kb.read_slots().await?;
    print_slots(&slots);
    Ok(())
}

pub async fn write(
    kb: &Keyboard,
    slot: u8,
    label: &str,
    secret_base32: &str,
    force: bool,
) -> Result<()> {
    let key = decode_base32_secret(secret_base32)?;

    let current = kb.read_slots().await?;
    if let Some(existing) = current.get(slot as usize).and_then(|s| s.as_ref()) {
        if !force && !confirm_overwrite(slot, existing)? {
            println!("aborted; slot {} unchanged", slot);
            return Ok(());
        }
    }

    let cmd = protocol::encode_write_slot(slot, label, &key)?;
    let updated = kb.apply(&cmd).await?;
    print_slots(&updated);
    Ok(())
}

pub async fn set_label(kb: &Keyboard, slot: u8, label: &str) -> Result<()> {
    let current = kb.read_slots().await?;
    if current.get(slot as usize).and_then(|s| s.as_ref()).is_none() {
        bail!("slot {} is empty - use `write` to provision it", slot);
    }
    let cmd = protocol::encode_set_label(slot, label)?;
    let updated = kb.apply(&cmd).await?;
    print_slots(&updated);
    Ok(())
}

pub async fn delete(kb: &Keyboard, slot: u8, force: bool) -> Result<()> {
    let current = kb.read_slots().await?;
    let existing = match current.get(slot as usize).and_then(|s| s.as_ref()) {
        Some(s) => s,
        None => {
            println!("slot {} already empty", slot);
            return Ok(());
        }
    };

    if !force && !confirm_delete(slot, existing)? {
        println!("aborted; slot {} unchanged", slot);
        return Ok(());
    }

    let cmd = protocol::encode_delete_slot(slot)?;
    let updated = kb.apply(&cmd).await?;
    print_slots(&updated);
    Ok(())
}

/// Exchange the key and label of two slots.
///
/// Nothing is lost either way round, so there's no confirmation prompt: a
/// mistaken swap is undone by running the same command again.
pub async fn swap(kb: &Keyboard, a: u8, b: u8) -> Result<()> {
    // Encoding first so an out-of-range index is rejected before we go over
    // the air to read state we'd only discard.
    let cmd = protocol::encode_swap_slots(a, b)?;
    if a == b {
        bail!("slot {} and slot {} are the same slot", a, b);
    }

    let current = kb.read_slots().await?;
    let occupant = |slot: u8| match current.get(slot as usize).and_then(|s| s.as_ref()) {
        Some(s) => format!("{:?}", s.label),
        None => "empty".to_owned(),
    };
    if current[a as usize].is_none() && current[b as usize].is_none() {
        println!("slots {} and {} are both empty; nothing to swap", a, b);
        return Ok(());
    }

    println!(
        "swapping slot {} ({}) with slot {} ({})",
        a,
        occupant(a),
        b,
        occupant(b)
    );
    let updated = kb.apply(&cmd).await?;
    print_slots(&updated);
    Ok(())
}

fn decode_base32_secret(secret: &str) -> Result<Vec<u8>> {
    let cleaned: String = secret
        .chars()
        .filter(|c| !c.is_whitespace() && *c != '-')
        .collect();
    let upper = cleaned.to_ascii_uppercase();
    base32::decode(base32::Alphabet::Rfc4648 { padding: false }, &upper)
        .ok_or_else(|| anyhow!("secret is not valid base32"))
}

fn confirm_overwrite(slot: u8, existing: &Slot) -> Result<bool> {
    let prompt = format!(
        "slot {} is occupied (label: {:?}). Overwrite?",
        slot, existing.label
    );
    Confirm::new()
        .with_prompt(prompt)
        .default(false)
        .interact()
        .map_err(Into::into)
}

fn confirm_delete(slot: u8, existing: &Slot) -> Result<bool> {
    let prompt = format!(
        "delete slot {} (label: {:?})? This is irreversible.",
        slot, existing.label
    );
    Confirm::new()
        .with_prompt(prompt)
        .default(false)
        .interact()
        .map_err(Into::into)
}

fn print_slots(slots: &[Option<Slot>; SLOT_COUNT]) {
    println!("slot  label");
    println!("----  -----");
    for (i, slot) in slots.iter().enumerate() {
        match slot {
            Some(s) => println!("{:>4}  {}", i, s.label),
            None => println!("{:>4}  -", i),
        }
    }
}
