---
name: wsl-usb-adb
description: Pass USB devices (especially Android phones for ADB) from Windows to WSL2 using usbipd-win. Use this skill when the user asks about using ADB in WSL, connecting Android devices in WSL2, USB passthrough from Windows to WSL, or mentions usbipd-win/ADB/USB in a WSL context.
metadata: {"nanobot":{"emoji":"🔌","requires":{"bins":["adb","usbip"]}}}
---

# WSL2 USB ADB via usbipd-win

Bridge USB Android devices from Windows into WSL2 for `adb` / `fastboot`.

## ⚠️ Who Does What

This setup requires actions on **both sides**. The split is non-negotiable because WSL2 has no direct access to physical USB controllers — only Windows does.

| Action | Where | Who | Automatable? |
|--------|-------|-----|--------------|
| Install usbipd-win | 💻 Windows | **User** | ❌ Manual (Windows installer) |
| `usbipd bind` (export device) | 💻 Windows PowerShell | **User** | ❌ Manual every time device is plugged / WSL restarts |
| `usbipd detach` (release device) | 💻 Windows PowerShell | **User** | ❌ Manual |
| Install linux-tools, adb | 🐧 WSL2 | Assistant | ✅ `apt-get install` |
| Kernel modules, symlink fix | 🐧 WSL2 | Assistant | ✅ |
| `usbip attach` (import device) | 🐧 WSL2 | `usb-attach` script | ✅ Semi-auto (script with auto-detection) |
| `adb devices / shell / install` | 🐧 WSL2 | User | N/A (normal usage) |

**Bottom line**: The assistant CAN automate everything on the WSL2 side. The Windows side steps are physical/manual and must be done by the user. Make this clear when instructing the user.

---

## Prerequisites

| Side | Package | Check |
|------|---------|-------|
| 💻 Windows | usbipd-win ≥ 4.0 | `usbipd list` in PowerShell |
| 🐧 WSL2 | linux-tools-generic + usbutils | `which usbip && which lsusb` |
| 🐧 WSL2 | adb | `adb version` |

---

## One-Time Setup

### 💻 Windows Side (User — Manual)

#### Install usbipd-win

In **Windows PowerShell (Admin)**:
```powershell
winget install --interactive --exact dorssel.usbipd-win
```

After installation, **restart WSL2** so the new USB/IP kernel support takes effect:
```powershell
wsl --shutdown
```
Then reopen your WSL terminal.

> ⚠️ This is the one and only step the assistant **cannot** do — it requires Windows GUI installer interaction.

---

### 🐧 WSL2 Side (Assistant — Can Fully Automate)

The following steps can all be executed by the assistant via shell commands.

#### Step 1: Install packages

```bash
sudo apt-get update && sudo apt-get install -y linux-tools-generic usbutils
```

#### Step 2: Load and auto-load kernel modules

```bash
echo -e "usbip-core\nvhci-hcd\nusbip-host" | sudo tee /etc/modules-load.d/usbip.conf
sudo modprobe usbip-core vhci-hcd usbip-host
```

#### Step 3: Fix usbip kernel version mismatch

WSL2 uses a custom kernel (e.g. `6.18.33.2-microsoft-standard-WSL2`) that differs from the Ubuntu linux-tools version. The `usbip` wrapper dispatches to `/usr/lib/linux-tools/<kernel>/` — create a symlink:

```bash
KERNEL_VER=$(uname -r)
TOOLS_VER=$(ls /usr/lib/linux-tools/ | head -1)
sudo ln -sf /usr/lib/linux-tools/$TOOLS_VER /usr/lib/linux-tools/$KERNEL_VER
```

Verify: `usbip version` should print `usbip (usbip-utils 2.0)` without warnings.

#### Step 4: (Optional) Avoid ADB port conflict

仅在 **Windows 和 WSL 两边同时使用 ADB** 时需要。WSL2 mirrored 模式下共享 `localhost`，如果 Windows ADB 占用了 5037，WSL ADB 需要换端口：

```bash
for rc in ~/.bashrc ~/.zshrc; do
    grep -q "ANDROID_ADB_SERVER_PORT" "$rc" 2>/dev/null || \
        echo 'export ANDROID_ADB_SERVER_PORT=5038' >> "$rc"
done
```

> 如果只用 WSL 跑 ADB，Windows 不跑，**跳过此步骤**，默认 5037 即可。

#### Step 5: Install `usb-attach` helper script

Write to `/usr/local/bin/usb-attach` (always in PATH, works for both bash and zsh):

```bash
#!/bin/bash
# WSL2 USB Attach — Import a USB Android device from Windows usbipd-win into WSL2.
# Usage: usb-attach [BUSID]
#   With BUSID: attaches that specific device.
#   Without:    auto-detects the first Android device exported by usbipd-win.

USBIPD_HOST="127.0.0.1"
BUSID="${1}"

# Check if device is already attached
EXISTING=$(sudo usbip port 2>/dev/null | grep -oP 'usbip://\K[0-9]+\.[0-9]+\.[0-9]+/[0-9]+-[0-9]+' | head -1)
if [ -n "$EXISTING" ]; then
    echo "✅ Device already attached: $EXISTING"
    adb start-server 2>/dev/null
    echo ""
    echo "📱 Connected devices:"
    adb devices
    exit 0
fi

if [ -z "$BUSID" ]; then
    echo "🔍 Checking for available devices on usbipd-win..."
    DEVICE_LIST=$(usbip list --remote="$USBIPD_HOST" 2>&1)

    if echo "$DEVICE_LIST" | grep -q "no exportable devices"; then
        echo ""
        echo "❌ No device exported on the Windows side yet."
        echo ""
        echo "👉 ACTION REQUIRED — Run these in 💻 Windows PowerShell (Admin):"
        echo ""
        echo "   usbipd list                          # Find your phone's BUSID"
        echo "   usbipd bind --busid <BUSID> --force  # Export the device"
        echo ""
        echo "   Then re-run: usb-attach"
        exit 1
    fi

    # Extract clean BUSID (strip trailing colon from usbip list output)
    BUSID=$(echo "$DEVICE_LIST" \
        | grep -iE "qualcomm|android|xiaomi|samsung|huawei|oneplus|google|pixel|oppo|vivo" \
        | grep -oE '[0-9]+-[0-9]+' | head -1)

    if [ -z "$BUSID" ]; then
        BUSID=$(echo "$DEVICE_LIST" | grep -oE '[0-9]+-[0-9]+' | head -1)
    fi

    if [ -z "$BUSID" ]; then
        echo "❌ Could not determine BUSID. Available devices:"
        echo "$DEVICE_LIST"
        exit 1
    fi

    echo "📱 Auto-detected BUSID: $BUSID"
fi

echo "🔌 Attaching BUSID=$BUSID from usbipd-win..."
sudo usbip attach --remote="$USBIPD_HOST" --busid="$BUSID" 2>&1

sleep 1

adb start-server 2>/dev/null

echo ""
echo "📱 Connected devices:"
adb devices
```

```bash
sudo chmod +x /usr/local/bin/usb-attach
```

---

## Daily Usage

### 💻 Step 1 — Windows PowerShell (Admin) [User — Manual]

**Every time** the phone is plugged in or WSL2 is restarted, the device must be exported from Windows:

```powershell
# List USB devices, find your phone's BUSID (e.g., 1-19)
usbipd list

# Export/bind the device so WSL can see it
usbipd bind --busid 1-19 --force
```

> 📌 **The BUSID may change** each time you replug the cable. Always check with `usbipd list` first.

### 🐧 Step 2 — WSL2 Terminal

```bash
# Auto-detect and attach
usb-attach

# Or with explicit BUSID:
usb-attach 1-19
```

### 🐧 Verify & Use

```bash
adb devices                 # Should show:  63DF24FA  device
adb shell                   # Open shell on phone
adb install app.apk         # Install APK
adb logcat                  # View logs
```

### Detach Device

```bash
# 🐧 WSL2: detach from WSL side
sudo usbip detach --port=0
```

```powershell
# 💻 Windows PowerShell (Admin): release from usbipd
usbipd detach --busid 1-19
```

---

## 🔁 Quick Reference Card

```
┌─────────────────────────────────────────────────────────────┐
│  💻 WINDOWS (User — Manual, cannot automate)                │
│                                                             │
│  Install:   winget install dorssel.usbipd-win               │
│  List:      usbipd list                                     │
│  Export:    usbipd bind --busid <X> --force                 │
│  Release:   usbipd detach --busid <X>                       │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│  🐧 WSL2 (Assistant can fully automate)                     │
│                                                             │
│  Import:    usb-attach [BUSID]                              │
│  Check:     adb devices                                     │
│  Detach:    sudo usbip detach --port=0                      │
└─────────────────────────────────────────────────────────────┘
```

---

## Troubleshooting

### ❌ "usbip not found for kernel X.X.X"

The usbip wrapper can't find binaries for the WSL2 custom kernel. **Fix**: run One-Time Setup Step 3 (symlink). This warning is cosmetic once fixed.

### ❌ "usb-attach: command not found"

Script is not in PATH. Install to `/usr/local/bin/` (not `~/.local/bin/` — zsh may not include it). See One-Time Setup Step 5.

### ❌ "Attach Request for X-X failed - Device not found"

The BUSID was parsed incorrectly (likely included a trailing colon like `1-19:` instead of `1-19`). The current script uses `grep -oE '[0-9]+-[0-9]+'` which avoids this. If still failing, pass BUSID manually: `usb-attach 1-19`.

### ⚠️ Device shows `?` (unauthorized)

**This does NOT block most ADB operations.** `adb push`, `adb shell`, `adb forward`, and `adb install` all work fine even with `?` status. Do not let this status deter you from proceeding — **always try the operation first** before diagnosing further.

If the operation actually fails with an authorization error (e.g., "device unauthorized"):
1. **Unlock the phone screen**
2. Look for **"Allow USB debugging?"** popup → tap **Allow** (✓ "Always allow")
3. If no popup: replug the USB cable, or go to **Developer Options → Revoke USB debugging authorizations** and replug
4. Then: `adb kill-server && adb start-server && adb devices`

### ❌ Device disappears after a while

USB/IP attachments are not persistent — they can drop on sleep/timeout. Simply re-run `usb-attach`. The device should still be exported on Windows side; verify with `usbip list --remote=127.0.0.1`.

### ❌ "Address already in use" (port 5037)

Port conflict with Windows ADB. Ensure `ANDROID_ADB_SERVER_PORT=5038` is exported in the current shell.

### ⚠️ "Device busy (exported)" — often a false alarm

If `usb-attach` reports `Attach Request for X-X failed - Device busy (exported)` but the device already appears in `adb devices`, it's because the device was **already attached** from a previous session. Check first:

```bash
sudo usbip port          # If device already listed here, no need to re-attach
export ANDROID_ADB_SERVER_PORT=5038
adb devices              # Device probably already works
```

Only if `sudo usbip port` shows nothing should you re-attach. If truly stuck (port shows nothing but attach fails), Windows may be holding the device — run in 💻 Windows PowerShell:
```powershell
adb kill-server
```

### ❌ After WSL2 restart

Two things must be re-done:
1. 💻 Windows PowerShell: `usbipd bind --busid <X> --force` (bindings don't survive WSL restart)
2. 🐧 WSL2: `usb-attach` (attachments don't survive WSL restart)

Kernel modules auto-load via `/etc/modules-load.d/usbip.conf` — no action needed for that.

### ❌ Windows interop broken (powershell.exe not callable from WSL)

This does **not** affect the setup. The `usb-attach` script uses the Linux `usbip` client to connect directly to `usbipd-win` at `127.0.0.1:3240` — it never calls Windows executables. This is by design.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  💻 WINDOWS                                               │
│                                                           │
│  Android Phone ──USB──▶ usbipd-win service               │
│                         ▲                                 │
│                         │ usbipd bind --busid X --force   │
│                         │ (USER MUST RUN MANUALLY)        │
│                         │                                 │
│                         │ Listens on TCP :3240            │
├─────────────────────────┼────────────────────────────────┤
│  🐧 WSL2 (mirrored net) │                                 │
│                         ▼                                 │
│  usbip attach --remote=127.0.0.1 --busid=X               │
│       │                                                   │
│       ▼                                                   │
│  vhci-hcd → /dev/bus/usb/002/002                         │
│       │                                                   │
│       ▼                                                   │
│  adb server (:5038) → adb devices → 63DF24FA  device     │
└──────────────────────────────────────────────────────────┘
```

## Key Lessons Learned

1. **Never rely on WSL interop** (`powershell.exe` from WSL). It may be broken. Use Linux `usbip` client against `127.0.0.1` instead.
2. **BUSID parsing**: `usbip list --remote=` outputs `1-19:` (with colon). Use `grep -oE '[0-9]+-[0-9]+'` — never `awk '{print $1}'`.
3. **Kernel version mismatch**: one symlink fixes it. Don't skip this.
4. **Install script to `/usr/local/bin/`** — it's always in PATH for bash and zsh. `~/.local/bin/` is not in zsh PATH by default.
5. **`ANDROID_ADB_SERVER_PORT=5038` is mandatory** in mirrored mode. Write it to both `.bashrc` and `.zshrc`.
6. **`usbip attach` needs `sudo`** — vhci-hcd requires root.
7. **Two-step workflow is inherent**: bind (Windows) → attach (WSL). Neither step is optional.
8. **Make Windows manual steps explicit**: Use `💻 Windows PowerShell (Admin)` markers. The assistant cannot run these — the user must.
9. **`?` device ≠ blocked**: `adb devices` showing `?` doesn't always mean unauthorized. Some devices (especially `userdebug`/`eng` builds, Qualcomm reference boards) show `?` but work fine. Always test with `adb shell echo "test"` before troubleshooting authorization.

## Verified Devices

Devices confirmed working through this setup:

| Device | USB ID | BUSID | Model | Notes |
|--------|--------|-------|-------|-------|
| Qualcomm Bengal | 05c6:901f | 1-19 | Bengal_for_arm64 | Shows `?` in adb devices but shell works |
