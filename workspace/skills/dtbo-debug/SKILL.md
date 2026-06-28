---
name: dtbo-debug
description: Debug Android/Linux device tree (DTB/DTBO) issues on Qualcomm devices. Use this skill when a peripheral (touchscreen, sensor, camera, etc.) stops working after flashing new firmware, or when there are GPIO conflicts, I2C device probe failures, or device tree misconfigurations. Covers: dmesg analysis, GPIO/IRQ/pinctrl comparison against schematics, dtbo unpacking/patching/repacking, and flashing via dd or fastboot.
---

# DTBO Debug & Patch

Debug hardware peripheral failures caused by Device Tree (DTB/DTBO) misconfiguration on Qualcomm Android/Linux devices, and apply surgical fixes without a full kernel rebuild.

## When to Use

- Touchscreen, sensor, camera, or other peripheral stopped working after firmware update
- dmesg shows driver probe failures (e.g., `probe of 0-XXXX failed with error -16`)
- GPIO conflicts between drivers
- Need to compare DT configuration against hardware schematics/pinout tables

## Workflow Overview

```
1. Collect Evidence  ──►  2. Locate Root Cause  ──►  3. Fix & Verify
   - dmesg | grep                 - Compare DTSI vs HW docs      - Edit DTSI/dts
   - gpio table                   - Check pinctrl conflicts       - Recompile DTBO
   - I2C device scan              - Verify IRQ assignments        - Flash & reboot
   - /sys/firmware/devicetree                                      - Confirm dmesg
```

---

## Step 1: Collect Evidence on Device

### 1.1 Driver Probe Logs

```bash
dmesg | grep -iE "fts|focal|goodix|synapt|novatek|ilitek|himax|touch|tp_probe|probe of"
```

Key patterns to look for:
- `[DRIVER/E]...probe fail` — driver failed (note error code: -16=EBUSY, -22=EINVAL)
- `probe of 0-XXXX failed with error -XX` — I2C device probe failure
- `gpio request failed` — GPIO already claimed by another driver

### 1.2 GPIO State

```bash
cat /sys/kernel/debug/gpio | grep -E "gpio(XX|YY|ZZ)"
```

Check:
- `func0` = GPIO mode, `func1` = peripheral function
- `in`/`out` direction matches expectation (IRQ should be `in`, reset should be `out`)
- `pull up`/`pull down` — IRQ pins usually need pull-up

### 1.3 I2C Bus Devices

```bash
ls -la /sys/bus/i2c/devices/
cat /sys/bus/i2c/devices/0-00XX/name  # identify chip
```

### 1.4 Input Devices

```bash
getevent -pl 2>/dev/null | grep -A8 -i "touch\|fts\|gt1x"
ls -la /dev/input/
```

### 1.5 Current Device Tree

```bash
find /sys/firmware/devicetree -path "*i2c*" -name "status"
find /sys/firmware/devicetree -path "*touch*" -o -path "*fts*" -o -path "*goodix*"
```

---

## Step 2: Compare Against Hardware Documentation

### 2.1 Extract Pin Assignments from DTSI/DTS

The DTSI file reveals what the kernel *expects*:

```dtsi
focaltech@38 {
    interrupts = <22 0x2008>;           // TLMM interrupt 22
    focaltech,reset-gpio = <&tlmm 72>;  // GPIO 72
    focaltech,irq-gpio = <&tlmm 88>;    // GPIO 88
};
```

And pinctrl definitions:
```dtsi
ts_int_active { pins = "gpio88"; };    // maps GPIO 88 to IRQ function
ts_reset_active { pins = "gpio72"; };   // maps GPIO 72 to reset function
```

### 2.2 Cross-Reference with Hardware Documents

| Source | Signal | Expected GPIO |
|--------|--------|---------------|
| Module GPIO config table (xlsx/pdf) | TP_INT | GPIO_80 |
| Module GPIO config table | TP_RST | GPIO_71 |
| Module GPIO config table | TP_I2C_SDA | GPIO_6 |
| Module GPIO config table | TP_I2C_SCL | GPIO_7 |
| Schematic (PDF) | Net labels matching above | Same GPIOs |

**Common mismatch scenarios:**
- DTSI uses wrong GPIO number (e.g., 88 vs actual 80)
- DTSI interrupt number doesn't match GPIO (e.g., `<22>` but should be `<80>`)
- Two drivers competing for the same GPIO (EBUSY -16)
- GPIO shared between unrelated peripherals (e.g., TP IRQ and SD card detect both on GPIO 88)

### 2.3 Check for GPIO Conflicts

```bash
# On device: find all DT nodes using a specific GPIO
grep "gpio88" -r /sys/firmware/devicetree/ 2>/dev/null
```

If `gpio88` appears in both `sdc2_cd` AND `ts_int_active`, that's a conflict.

---

## Step 3: Unpack and Patch the DTBO Image

Since full kernel rebuild is slow, patch the running DTBO image directly.

### 3.1 Pull DTBO from Device

```bash
# On device (via device_shell)
dd if=/dev/block/by-name/dtbo_a of=/data/local/tmp/dtbo.img bs=4k

# Download to host
# (or copy via adb pull / device_download)
```

Identify partition: `ls -la /dev/block/by-name/ | grep dtbo`

### 3.2 Parse DTBO Header (Python)

```python
import struct

with open('dtbo.img', 'rb') as f:
    data = f.read()

# DTBO header is BIG-ENDIAN
magic           = struct.unpack_from('>I', data, 0)[0]   # 0xD7B7AB1E
total_size      = struct.unpack_from('>I', data, 4)[0]
header_size     = struct.unpack_from('>I', data, 8)[0]   # typically 32
dt_entry_size   = struct.unpack_from('>I', data, 12)[0]  # typically 32
dt_entry_count  = struct.unpack_from('>I', data, 16)[0]
dt_entries_off  = struct.unpack_from('>I', data, 20)[0]

for i in range(dt_entry_count):
    off = dt_entries_off + i * dt_entry_size
    entry_size   = struct.unpack_from('>I', data, off)[0]
    entry_offset = struct.unpack_from('>I', data, off + 4)[0]
    entry_id     = struct.unpack_from('>I', data, off + 8)[0]
    # Save DTB, decompile with dtc
```

### 3.3 Decompile, Modify, Recompile

```bash
# Decompile each DTB entry
dtc -I dtb -O dts -o entry_XX.dts entry_XX.dtb

# Edit the DTS — fix GPIO numbers
# Example: <0x16 0x2008> → <0x50 0x2008> (GPIO 22→80)
#          <0xFFFFFFFF 0x48> → <0xFFFFFFFF 0x47> (GPIO 72→71)
#          <0xFFFFFFFF 0x58> → <0xFFFFFFFF 0x50> (GPIO 88→80)

# Recompile
dtc -I dts -O dtb -o entry_XX_fixed.dtb entry_XX_fixed.dts
```

**GPIO hex conversion:**
| GPIO | Hex |
|------|-----|
| 22   | 0x16 |
| 71   | 0x47 |
| 72   | 0x48 |
| 80   | 0x50 |
| 88   | 0x58 |

### 3.4 Repack DTBO

Update entry sizes in header, pad new DTBs to original size, and write back:

```python
# Update entry dt_size
struct.pack_into('>I', data, off, new_size)  # entry size in header

# Pad to original entry size, place back
padded = new_dtb + b'\x00' * (old_size - new_size)
data[entry_offset:entry_offset + old_size] = padded

# Write output
with open('dtbo_fixed.img', 'wb') as f:
    f.write(data)
```

---

## Step 4: Flash and Verify

### 4.1 Flash to Device

```bash
# Method A: Direct dd (requires root, bootloader unlocked)
dd if=/data/local/tmp/dtbo_fixed.img of=/dev/block/by-name/dtbo_a bs=4k
sync && reboot

# Method B: Fastboot
adb reboot bootloader
fastboot flash dtbo_a dtbo_fixed.img
fastboot reboot
```

**Important:** Check bootloader lock state first:
```bash
getprop ro.boot.verifiedbootstate  # "orange" = unlocked (safe to dd)
getprop ro.boot.flash.locked       # "0" = unlocked
```

If locked (green), AVB will reject modified images — use fastboot with signed images.

### 4.2 Verify After Reboot

```bash
# 1. No more probe errors
dmesg | grep -iE "fts|focal|goodix|probe of|fail"

# 2. Input device appears
getevent -pl | grep -A8 -i touch
ls -la /dev/input/

# 3. I2C device is active
cat /sys/bus/i2c/devices/0-0038/name

# 4. GPIOs are correct
cat /sys/kernel/debug/gpio | grep -E "gpio(71|80)"
```

### 4.3 Rollback

```bash
# If device fails to boot or peripheral still broken:
dd if=/data/local/tmp/dtbo_backup.img of=/dev/block/by-name/dtbo_a bs=4k
reboot
```

---

## Common Error Codes

| Code | Name | Typical Cause |
|------|------|---------------|
| -16  | EBUSY | GPIO/IRQ already claimed by another driver |
| -22  | EINVAL | Invalid DT property or missing required config |
| -517 | EPROBE_DEFER | Dependency not ready (usually self-corrects) |

---

## Diagnostic Checklist

- [ ] `dmesg` shows driver probe attempted
- [ ] I2C device responds at expected address
- [ ] GPIO state matches expected function (IRQ=in, RST=out)
- [ ] No GPIO conflicts between DT nodes
- [ ] Pinctrl definitions match hardware pinout
- [ ] DTBO decompiles without errors
- [ ] Patched DTS recompiles cleanly
- [ ] `getevent` shows touch input device after fix
- [ ] No regression in other peripherals (SD card, NFC, etc.)

---

## Tools Required

| Tool | Purpose |
|------|---------|
| `dtc` | Decompile/compile DTB/DTS |
| `python3` | Parse DTBO header, patch binary |
| `dd` | Flash partitions on device |
| `fastboot` | Alternative flash method |
| `dmesg` | Kernel log analysis |
| `getevent` | Input device verification |

Install `dtc` on host: `apt install device-tree-compiler`
