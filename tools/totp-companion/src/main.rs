mod ble;
mod commands;
mod protocol;

use anyhow::Result;
use clap::{Parser, Subcommand};

use crate::ble::Keyboard;

const DEFAULT_NAME: &str = "Keyball39";

#[derive(Parser)]
#[command(
    name = "keyball39-totp",
    about = "Provision TOTP slots on a Keyball39 over BLE.",
    long_about = None,
)]
struct Cli {
    /// Advertised BLE name of the keyboard to target.
    #[arg(long, global = true, default_value = DEFAULT_NAME)]
    name: String,

    #[command(subcommand)]
    cmd: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Show all 30 slots and their labels.
    List,
    /// Write a new key (or overwrite an existing one).
    Write {
        /// Slot index, 0..15.
        slot: u8,
        /// Label, up to 16 bytes UTF-8.
        label: String,
        /// Base32-encoded secret (whitespace and dashes ignored).
        secret: String,
        /// Don't prompt before overwriting.
        #[arg(long)]
        force: bool,
    },
    /// Rename an existing slot.
    SetLabel {
        slot: u8,
        label: String,
    },
    /// Erase a slot.
    Delete {
        slot: u8,
        /// Don't prompt before deleting.
        #[arg(long)]
        force: bool,
    },
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    let kb = Keyboard::connect(&cli.name).await?;
    let result = dispatch(&kb, cli.cmd).await;
    kb.disconnect().await.ok();
    result
}

async fn dispatch(kb: &Keyboard, cmd: Command) -> Result<()> {
    match cmd {
        Command::List => commands::list(kb).await,
        Command::Write {
            slot,
            label,
            secret,
            force,
        } => commands::write(kb, slot, &label, &secret, force).await,
        Command::SetLabel { slot, label } => commands::set_label(kb, slot, &label).await,
        Command::Delete { slot, force } => commands::delete(kb, slot, force).await,
    }
}
