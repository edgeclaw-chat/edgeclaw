---
name: wsl-usb-adb
description: Pass USB devices (especially Android phones for ADB) from Windows to WSL2 using usbipd-win. Use this skill when the user asks about using ADB in WSL, connecting Android devices in WSL2, USB passthrough from Windows to WSL, or mentions usbipd-win/ADB/USB in a WSL context.
metadata: {"nanobot":{"emoji":"🔌","requires":{"bins":["adb","usbip","powershell.exe"]}}}
---

# WSL2 USB ADB via usbipd-win

Bridge USB Android devices from Windows into WSL2 for `adb` / `fastboot`.

## Prerequisites

| Side      | Package            | Check                                   |
|-----------|--------------------|-----------------------------------------|
| Windows   | usbipd-win ≥ 4.0   | `usbipd list` in PowerShell             |
| WSL2      | linux-tools-generic | `which usbip`                           |
| WSL2      | adb                | `adb version`                           |

WSL2 kernel modules must load: `usbip-core`, `vhci-hcd`, `usbip-host`.

---

## One-Time Setup

### 1. Install usbip client in WSL2

```bash
sudo apt-get update && sudo apt-get install -y linux-tools-generic
```

### 2. Auto-load kernel modules

```bash
echo -e "usbip-core\nvhci-hcd\nusbip-host" | sudo tee /etc/modules-load.d/usbip.conf
sudo modprobe usbip-core vhci-hcd usbip-host
```

### 3. Avoid port conflict (WSL2 mirrored networking)

Windows ADB often holds port 5037. Tell WSL ADB to use 5038:

```bash
echo 'export ANDROID_ADB_SERVER_PORT=5038' >> ~/.bashrc
```

### 4. Install helper script

Save as `~/.local/bin/usb-attach`:

```bash
#!/bin/bash
# Auto-detect Android device and attach via usbipd-win
BUSID="${1}"

if [ -z "$BUSID" ]; then
    DEVICE_LINE=$(powershell.exe -Command "& 'C:\Program Files\usbipd-win\usbipd.exe' list" 2>/dev/null \
        | grep -iE "qualcomm|android|xiaomi|samsung|huawei|oneplus|google|pixel|oppo|vivo")
    if [ -z "$DEVICE_LINE" ]; then
        echo "❌ No Android device found. Run: usbipd list  (in PowerShell)"
        exit 1
    fi
    BUSID=$(echo "$DEVICE_LINE" | awk '{print $1}')
    echo "📱 $DEVICE_LINE"
fi

powershell.exe -Command "& 'C:\Program Files\usbipd-win\usbipd.exe' bind --busid '$BUSID' --force" 2>/dev/null
powershell.exe -Command "& 'C:\Program Files\usbipd-win\usbipd.exe' attach --wsl --busid '$BUSID'" 2>/dev/null

sleep 1
export ANDROID_ADB_SERVER_PORT=5038
adb start-server 2>/dev/null
adb devices
```

Make executable: `chmod +x ~/.local/bin/usb-attach`

---

## Daily Usage

### Attach device

```bash
usb-attach
```

Or manually with a known BUSID:

```bash
usb-attach 1-19
```

### Verify

```bash
adb devices
# List of devices attached
# 63DF24FA    device
```

### Detach (return device to Windows)

In **PowerShell (Admin)**:
```powershell
usbipd detach --busid 1-19
```

---

## Troubleshooting

### "Device busy (exported)"
Windows is holding the device. Kill adb on Windows first:
```powershell
adb kill-server
```
Then rebind and attach.

### "could not install *smartsocket* listener: Address already in use"
Port 5037 is taken. Use `ANDROID_ADB_SERVER_PORT=5038` (or `adb -P 5038`).

### "usbip not found for kernel X.X.X"
WSL2 custom kernel mismatch — `linux-tools-generic` still provides a working `usbip` binary. Ignore if `lsusb` shows the device.

### Device shows `?` in `adb devices`
The phone needs USB debugging authorization. Unlock phone and tap **"Allow USB debugging"** dialog. If no dialog appears:
```bash
adb kill-server && adb start-server
adb devices
```

### After WSL2 restart
Kernel modules auto-load via `/etc/modules-load.d/`, but you must re-run `usb-attach` to re-attach the device.

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│  Windows                                         │
│  ┌──────────┐    ┌──────────────┐               │
│  │ USB Host │───▶│ usbipd-win   │               │
│  │ (Phone)  │    │ bind+attach  │               │
│  └──────────┘    └──────┬───────┘               │
│                         │ TCP 3240              │
├─────────────────────────┼───────────────────────┤
│  WSL2 VM (mirrored net) │                       │
│  ┌──────────────────────▼──────────────────────┐│
│  │  vhci-hcd (virtual USB host controller)     ││
│  │  → /dev/bus/usb/002/002                    ││
│  │  → lsusb: Qualcomm QCM2290-IDP             ││
│  └──────────────────────┬──────────────────────┘│
│                         ▼                       │
│  ┌──────────────────────────────────────────┐  │
│  │  adb server (port 5038)                   │  │
│  │  adb devices → 63DF24FA  device          │  │
│  └──────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
```

## Notes

- Works only on **WSL2** (WSL1 has direct USB and doesn't need usbipd)
- Mirrored networking mode (`wsl.conf` `[wsl2] networkingMode=mirrored`) enables `bind` from WSL side via PowerShell
- Fastboot also works once the device is in bootloader mode and attached via usbipd
- Multiple devices can be attached simultaneously (different BUSIDs)
