---
name: dtbo-debug
description: Debug and fix peripheral failures caused by Device Tree misconfiguration on Linux/Android devices. Use this skill when any peripheral (touchscreen, sensor, camera, audio codec, etc.) stops working after a firmware change, or when dmesg shows driver probe failures, GPIO conflicts, or I2C/SPI errors. Covers the full cycle: evidence collection, DTS vs hardware cross-reference, DTBO/DTB binary patching without full kernel rebuild, and flash verification.
---

# DTBO/DTS Debug & Patch

Debug hardware peripheral failures rooted in Device Tree misconfiguration, and apply targeted fixes without a full kernel rebuild.

## When to Use

- **Any peripheral** stops working after a firmware/dtb update
- `dmesg` shows driver `probe` failures (e.g., `error -16`, `error -22`)
- GPIO, IRQ, or pinmux conflicts between drivers
- Need to verify DT configuration against hardware documentation

> This skill is **hardware-agnostic** — it works for touchscreens, sensors, cameras, audio codecs, NFC, modems, or any device described in Device Tree.

---

## Workflow Overview

```
1. Evidence         2. Root Cause          3. Fix & Deploy
   dmesg│gpio│i2c     DT vs HW docs          patch│repack│flash
   ──────────────────────────────────────────────────────────
      On Device            On Host              Host + Device
```

---

## Step 1: Collect Evidence (on Device)

Start broad, then narrow based on what the affected peripheral is.

### 1.1 Kernel Messages — Find the Failure

```bash
# Driver probe failures
dmesg | grep -iE "probe of|failed|error -[0-9]"

# Per-peripheral filter (adapt keywords to your hardware)
dmesg | grep -iE "<driver_name>|<subsystem>|i2c|gpio|pinctrl"
```

**What to look for:**

| Pattern | Meaning |
|---------|---------|
| `probe of XXXX failed with error -16` | EBUSY — resource (GPIO/IRQ/regulator) already claimed |
| `probe of XXXX failed with error -22` | EINVAL — invalid or missing DT property |
| `gpio request failed` | GPIO conflict with another driver |
| `-517` (EPROBE_DEFER) | Dependency not ready yet (may self-resolve) |
| No probe message at all | Driver not bound — check `status = "okay"` in DT |

### 1.2 GPIO State

```bash
cat /sys/kernel/debug/gpio | grep -E "gpio(<NN>|<MM>)"
```

Check:
- **Direction**: IRQ pins should be `in`, reset/enable pins should be `out`
- **Function**: `func0` = GPIO mode, `func1`/`func2` = peripheral mux
- **Pull**: IRQ pins typically need pull-up; reset lines vary

### 1.3 Bus Topology — Is the Device Reachable?

```bash
# I2C devices
ls -la /sys/bus/i2c/devices/
cat /sys/bus/i2c/devices/<bus>-<addr>/name

# SPI devices
ls -la /sys/bus/spi/devices/

# Check if the bus itself is active
cat /sys/firmware/devicetree/base/soc/<bus-node>/status
```

If the device responds on the bus but the driver fails, the problem is likely in **control signals** (GPIO/IRQ), not the bus itself.

### 1.4 Verify What Actually Loaded

```bash
# Running device tree (what the kernel actually sees)
find /sys/firmware/devicetree -path "*<your-device>*"

# Check node status
cat /sys/firmware/devicetree/.../status   # "okay" or "disabled"

# Input subsystem (for HID/touch)
getevent -pl 2>/dev/null
ls -la /dev/input/
```

---

## Step 2: Find the Root Cause (on Host)

### 2.1 The Core Question

> **Does the Device Tree match the actual hardware?**

Three sources of truth, two in software, one in hardware:

| Source | What it tells you | Location |
|--------|-------------------|----------|
| **DTSI/DTS source** | What the kernel *expects* | Kernel tree or doc archive |
| **Running Device Tree** | What the kernel *actually loaded* | `/sys/firmware/devicetree` on device |
| **Hardware documentation** | What the PCB *actually connects* | Schematics, pinout tables, datasheets |

If any two disagree, the peripheral won't work.

### 2.2 Extract Signal Assignments from Hardware Documents

The DTSI side is plain text — but hardware docs come in PDF schematics, Excel pinout tables, or HTML datasheets. Extract the relevant signals before attempting comparison.

#### 2.2.1 Schematics (PDF)

Use `pdftotext` to pull net names from schematic PDFs:

```bash
# Search for the peripheral's signal nets
pdftotext schematic.pdf - | grep -iE "tp_int|tp_rst|sensor_int|<signal_name>"

# Or broader: find all nets on a specific page/block
pdftotext schematic.pdf - | grep -iE "<peripheral>|i2c|<bus_name>" | head -40
```

**What to look for:**
- Net labels like `TP_INT`, `TP_RST`, `TP_I2C_SDA`, `CAM_PWDN`
- Pin numbers on connectors between the SoC/module and the peripheral
- Supply rails: `VDD_3V3`, `VIO_1V8` → verify regulator assignment in DTSI

**Tip:** If `pdftotext` output is jumbled, search for connector designators (e.g., `J0801`) and trace the pins manually from the PDF with a viewer.

#### 2.2.2 GPIO Pinout Tables (Excel/CSV)

Module vendors often provide GPIO configuration spreadsheets. Parse them programmatically:

```python
import openpyxl  # or csv, pandas

wb = openpyxl.load_workbook('gpio_config.xlsx')
for sheet_name in wb.sheetnames:
    ws = wb[sheet_name]
    for row in ws.iter_rows(values_only=True):
        row_str = '|'.join([str(c or '') for c in row])
        # Filter for the peripheral's signals
        if any(k in row_str.lower() for k in ['tp', 'touch', 'sensor', 'camera']):
            print(' | '.join([str(c or '') for c in row]))
```

**What the table typically contains:**

| Pin# | Signal Name | GPIO | Function | Voltage | Notes |
|------|------------|------|----------|---------|-------|
| 30 | TP_INT | GPIO_80 | TP interrupt | 1.8V | QUP SE2 |
| 31 | TP_RST | GPIO_71 | TP reset | 1.8V | |
| 47 | TP_I2C_SCL | GPIO_7 | I2C clock | 1.8V | Needs external pull-up |

Extract the **Signal Name → GPIO** mapping. This is the hardware truth that the DTSI must match.

#### 2.2.3 Board-Specific vs Module-Level

In modular designs (SoM + carrier board), pin assignments exist at **two levels**:

```
SoC ball → Module pin (fixed, from module datasheet)
Module pin → Peripheral (from carrier board schematic)
```

The DTSI must match the **SoC ball / GPIO number**, while the schematic shows the **module pin**. Cross-reference using the module's GPIO configuration table as the bridge:

```
Schematic net:  TP_INT → Module Pin 30
GPIO config:    Pin 30 → GPIO_80  ← This is what the DTSI needs
```

### 2.3 Cross-Reference Method

For each signal of the failing peripheral, build a comparison table:

```
Signal   | DTSI says    | HW doc says  | Match?
---------|-------------|-------------|--------
IRQ      | GPIO_88     | GPIO_80     | ❌
RESET    | GPIO_72     | GPIO_71     | ❌
SDA      | GPIO_6      | GPIO_6      | ✅
SCL      | GPIO_7      | GPIO_7      | ✅
```

Common mismatches:
- **Wrong GPIO number** — DTSI pin doesn't match the module/SoC pinout
- **Wrong interrupt specifier** — `interrupts = <22>` but the actual IRQ pin is GPIO 80
- **Pinctrl not matching** — pinctrl definitions reference different pins than the driver node
- **Shared GPIO** — two peripherals (e.g., touchscreen IRQ and SD card detect) assigned the same GPIO
- **Wrong function** — GPIO muxed as `func1` (peripheral) but driver expects `func0` (GPIO mode)

### 2.4 Check for Resource Conflicts

```bash
# On device: which DT nodes reference a suspicious GPIO?
grep -r "gpio<XX>" /sys/firmware/devicetree/ 2>/dev/null | grep -v phandle
```

If GPIO_N appears under two unrelated devices (e.g., both `sdc2_cd` and `ts_int`), that's a conflict — whichever driver probes first wins, the second gets -EBUSY.

### 2.5 Pinctrl Verification

Pinctrl definitions (in `*-pinctrl.dtsi`) set up pinmux and pull configuration. They must match both the driver node's GPIO references **and** the physical pin:

```dtsi
/* Driver node says: */
my_device: my_device@38 {
    my_device,irq-gpio = <&tlmm 80 0>;  /* GPIO 80 */
    pinctrl-0 = <&my_int_active>;        /* references pinctrl state */
};

/* Pinctrl must agree: */
my_int_active: my_int_active {
    pins = "gpio80";   /* ← must be the same GPIO */
    function = "gpio";  /* ← GPIO mode, not peripheral */
    bias-pull-up;       /* ← appropriate for IRQ */
};
```

If `my_device,irq-gpio` uses GPIO 80 but `my_int_active` configures GPIO 88, the IRQ signal never reaches the driver.

---

## Step 3: Fix Without Full Rebuild

Full kernel rebuild is slow (~hours). Patch the DTBO/DTB binary directly (~minutes).

### 3.1 Determine Where the DT Lives

```bash
# On device
ls -la /dev/block/by-name/ | grep -E "dtbo|dtb|boot"
cat /proc/partitions | grep -E "dtbo|dtb|boot"
```

| Partition | Typical Content |
|-----------|----------------|
| `dtbo_a`/`dtbo_b` | Device Tree Blob Overlay (A/B devices) |
| `dtb` | Single Device Tree Blob |
| `boot_a`/`boot_b` | Kernel + DTB + initramfs |

### 3.2 Pull the Image

```bash
# On device: copy partition to file
dd if=/dev/block/by-name/<dtbo_partition> of=/data/local/tmp/dtbo.img bs=4k

# On host: pull the file
adb pull /data/local/tmp/dtbo.img .
```

Always **keep a backup** — this is your rollback.

### 3.3 Check Image Format

```bash
od -A x -t x1 -N 4 dtbo.img | head -1
# 0xD7B7AB1E → DTBO table (big-endian, multi-overlay)
# 0xD00DFEED → Single FDT/DTB
# 0xANDROID!  → Android boot image (contains DTB inside)
```

### 3.4 Unpack → Modify → Repack

**For DTBO images** (multiple overlays in a table):

```python
import struct

with open('dtbo.img', 'rb') as f:
    data = bytearray(f.read())

# DTBO header is BIG-ENDIAN
dt_entry_count = struct.unpack_from('>I', data, 16)[0]
dt_entry_size  = struct.unpack_from('>I', data, 12)[0]
dt_entries_off = struct.unpack_from('>I', data, 20)[0]

for i in range(dt_entry_count):
    off = dt_entries_off + i * dt_entry_size
    size   = struct.unpack_from('>I', data, off)[0]
    offset = struct.unpack_from('>I', data, off + 4)[0]
    # Extract DTB → decompile with dtc
```

**For any DTB/DTBO entry**: decompile, edit, recompile:

```bash
dtc -I dtb -O dts -o original.dts original.dtb   # decompile
# Edit original.dts → fix GPIO/IRQ/pinctrl
dtc -I dts -O dtb -o fixed.dtb fixed.dts         # recompile
```

Then update entry size in header, pad to original size, write back.

### 3.5 Decision: Which Entries to Patch?

Not all entries in a DTBO apply to your board. Find the active one(s):

```bash
# Decompile all entries, grep for the failing driver
for dts in entry_*.dts; do
    grep -l "<driver_compatible_string>" "$dts"
done
```

Patch only entries that:
1. Contain the failing peripheral's DT node
2. Have `status = "okay"` (not `"disabled"`)

---

## Step 4: Deploy & Verify

### 4.1 Check Bootloader State

```bash
getprop ro.boot.verifiedbootstate   # "orange" = unlocked, "green" = locked
getprop ro.boot.flash.locked        # "0" = unlocked
```

- **Unlocked**: `dd` direct write works
- **Locked**: AVB will reject modified images; use fastboot with proper signing, or unlock bootloader first

### 4.2 Flash

```bash
# Unlocked bootloader: direct write
dd if=/data/local/tmp/dtbo_fixed.img of=/dev/block/by-name/<partition> bs=4k
sync && reboot

# Fastboot (requires bootloader mode)
adb reboot bootloader
fastboot flash <partition> dtbo_fixed.img
fastboot reboot
```

### 4.3 Verify After Reboot

```bash
# 1. No more probe errors
dmesg | grep -iE "probe of|failed|error -"

# 2. Device node / sysfs entry appears
ls /sys/bus/i2c/devices/<addr>/
cat /sys/bus/i2c/devices/<addr>/name

# 3. Functional test (domain-specific)
getevent -pl | grep <device>          # input devices
cat /dev/<device_node>                 # char devices
```

### 4.4 Rollback If Needed

```bash
dd if=/data/local/tmp/dtbo_backup.img of=/dev/block/by-name/<partition> bs=4k
reboot
```

---

## Diagnostic Checklist (Fit to Your Scenario)

- [ ] Driver probe was attempted (`dmesg` shows driver init messages)
- [ ] Peripheral responds on the bus (`/sys/bus/.../devices/` shows address)
- [ ] Bus status is `okay` in DT
- [ ] Each control GPIO direction matches expected function
- [ ] No GPIO/IRQ conflicts with other drivers
- [ ] Pinctrl definitions match the GPIOs declared in the driver node
- [ ] Hardware docs (schematics/pinout) agree with DT assignments
- [ ] After fix: no probe errors, device node present, functional test passes
- [ ] Other peripherals unaffected (spot-check SD card, buttons, etc.)

---

## Common Error Codes

| Code | Name | Typical Root Cause |
|------|------|--------------------|
| -16  | EBUSY | GPIO/IRQ/regulator already claimed by another driver |
| -22  | EINVAL | Invalid or missing DT property; pinctrl mismatch |
| -517 | EPROBE_DEFER | Dependency not probed yet (regulator, clock, GPIO controller) |

---

## Tools Required

| Tool | Purpose | Install |
|------|---------|---------|
| `dtc` | Decompile/compile DTB ↔ DTS | `apt install device-tree-compiler` |
| `python3` | Parse DTBO headers, binary patching | (system built-in) |
| `dmesg` | Kernel log | (on device) |
| `dd` / `fastboot` | Flash partitions | (on device / host) |
