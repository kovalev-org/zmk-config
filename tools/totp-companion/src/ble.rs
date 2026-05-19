//! Locate the Keyball39's right half among OS-paired BLE devices and expose a
//! small RPC surface over its custom TOTP GATT service.
//!
//! Uses `bluest` because on Windows the keyboard is already-paired and
//! actively connected for HID, so it isn't advertising — only a paired-device
//! enumeration (WinRT) finds it, not an advertisement scan.

use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Context, Result};
use bluest::{Adapter, Characteristic, Device};
use futures_util::StreamExt;
use tokio::time::{sleep, timeout};

use crate::protocol::{
    self, Slot, COMMAND_CHAR_UUID, SERVICE_UUID, SLOTS_CHAR_UUID, SLOT_COUNT,
};

/// How long to wait for the adapter to be powered on.
const ADAPTER_READY_TIMEOUT: Duration = Duration::from_secs(5);
/// How long to wait for the device to surface (paired-or-advertising).
const FIND_DEVICE_TIMEOUT: Duration = Duration::from_secs(10);
/// Brief settle delay between a mutating write and the follow-up read, so the
/// keyboard has time to finalise the flash save before we read back.
const POST_WRITE_SETTLE: Duration = Duration::from_millis(50);

pub struct Keyboard {
    /// Held to keep the GATT session alive. bluest disconnects on drop.
    _device: Device,
    command: Characteristic,
    slots: Characteristic,
}

impl Keyboard {
    /// Find the first paired-or-connected device whose name matches `name`,
    /// connect (if not already), locate the TOTP service, and push host time.
    pub async fn connect(name: &str) -> Result<Self> {
        let adapter = Adapter::default()
            .await
            .ok_or_else(|| anyhow!("no BLE adapter found"))?;
        timeout(ADAPTER_READY_TIMEOUT, adapter.wait_available())
            .await
            .context("adapter not available within timeout")?
            .context("adapter became unavailable")?;

        let device = find_device(&adapter, name).await?;

        if !device.is_connected().await {
            adapter
                .connect_device(&device)
                .await
                .context("connect to device")?;
        }

        let (command, slots) = find_chars(&device).await?;
        let kb = Keyboard {
            _device: device,
            command,
            slots,
        };
        kb.push_time().await?;
        Ok(kb)
    }

    pub async fn push_time(&self) -> Result<()> {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .context("system clock is before unix epoch")?
            .as_secs();
        let buf = protocol::encode_set_time(now);
        self.write_command(&buf).await
    }

    pub async fn read_slots(&self) -> Result<[Option<Slot>; SLOT_COUNT]> {
        let bytes = self
            .slots
            .read()
            .await
            .context("read slots characteristic")?;
        protocol::decode_slots(&bytes)
    }

    /// Send a mutating command, then read back the slots characteristic.
    ///
    /// We don't use the NOTIFY path because the slots payload (272 bytes)
    /// exceeds the default ATT notify limit (ATT_MTU-3 ≈ 20 bytes), so the
    /// keyboard's `bt_gatt_notify` silently fails. Reads, by contrast, use
    /// ATT_READ_BLOB_REQ to fetch long values in chunks transparently.
    pub async fn apply(&self, command: &[u8]) -> Result<[Option<Slot>; SLOT_COUNT]> {
        self.write_command(command).await?;
        sleep(POST_WRITE_SETTLE).await;
        self.read_slots().await
    }

    pub async fn disconnect(self) -> Result<()> {
        // bluest disconnects on Device drop; explicit disconnect via adapter
        // requires holding the adapter handle, which we don't here. The OS
        // keeps the HID link alive; only our GATT session goes away.
        drop(self);
        Ok(())
    }

    async fn write_command(&self, bytes: &[u8]) -> Result<()> {
        self.command
            .write(bytes)
            .await
            .context("write command characteristic")
    }
}

async fn find_device(adapter: &Adapter, name: &str) -> Result<Device> {
    // First try already-connected devices — that's the common case when the
    // keyboard is paired and typing.
    if let Ok(connected) = adapter.connected_devices().await {
        if let Some(d) = match_by_name(&connected, name).await {
            return Ok(d);
        }
    }

    // Then try OS-known devices that may not be connected right now.
    if let Ok(devices) = adapter
        .connected_devices_with_services(&[SERVICE_UUID])
        .await
    {
        if let Some(d) = match_by_name(&devices, name).await {
            return Ok(d);
        }
    }

    // Last resort: do a short advertisement scan in case the keyboard happens
    // to be disconnected and advertising.
    let scan = adapter
        .scan(&[])
        .await
        .context("start advertisement scan")?;
    tokio::pin!(scan);

    let scanned = timeout(FIND_DEVICE_TIMEOUT, async {
        while let Some(adv) = scan.next().await {
            if adv.adv_data.local_name.as_deref() == Some(name) {
                return Ok::<_, anyhow::Error>(adv.device);
            }
        }
        Err(anyhow!("scan stream ended"))
    })
    .await
    .map_err(|_| {
        anyhow!(
            "no BLE device named {:?} found within {:?}. Is it paired and powered?",
            name,
            FIND_DEVICE_TIMEOUT
        )
    })??;

    Ok(scanned)
}

async fn match_by_name(devices: &[Device], name: &str) -> Option<Device> {
    for d in devices {
        if let Ok(n) = d.name() {
            if n == name {
                return Some(d.clone());
            }
        }
    }
    None
}

async fn find_chars(device: &Device) -> Result<(Characteristic, Characteristic)> {
    let services = device
        .discover_services_with_uuid(SERVICE_UUID)
        .await
        .context("discover services")?;
    let service = services.into_iter().next().ok_or_else(|| {
        anyhow!(
            "device does not expose TOTP service {} - firmware out of date?",
            SERVICE_UUID
        )
    })?;

    let command = service
        .discover_characteristics_with_uuid(COMMAND_CHAR_UUID)
        .await
        .context("discover command characteristic")?
        .into_iter()
        .next()
        .ok_or_else(|| anyhow!("missing command characteristic"))?;

    let slots = service
        .discover_characteristics_with_uuid(SLOTS_CHAR_UUID)
        .await
        .context("discover slots characteristic")?
        .into_iter()
        .next()
        .ok_or_else(|| anyhow!("missing slots characteristic"))?;

    Ok((command, slots))
}
