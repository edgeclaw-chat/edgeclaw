---
name: adb-device-agent
description: Build, deploy, and run the edgeclaw device agent on Android devices via ADB. Use this skill when the user asks to deploy the agent to a device, set up device shell, or connect edgeclaw to an Android device over ADB.
---

# ADB Device Agent Deployment

Build the edgeclaw device agent and deploy it to an Android device via ADB for remote shell execution.

## Architecture

```
┌──────────────┐     adb forward      ┌─────────────────────┐
│   edgeclaw   │ ◄──────────────────► │   Android Device    │
│  (host)      │   tcp:5000 ↔ tcp:5000│                     │
│  device_shell│                      │  device_agent       │
│     tool     │──HTTP POST /exec────►│  --socket           │
│              │◄──JSON response──────│  0.0.0.0:5000       │
└──────────────┘                      └─────────────────────┘
```

- `device_agent` listens on a TCP port in socket mode
- `adb forward` maps host port to device port
- edgeclaw's `device_shell` tool sends HTTP requests to `/exec`

## Prerequisites

- ADB connected to target device
- Bazel build system
- If using WSL2: USB device passed through from Windows (see wsl-usb-adb skill)

## Workflow

### Step 1: Build

```bash
cd /path/to/edgeclaw
bazel build //src:device_agent
```

Binary at `bazel-bin/src/device_agent` (ARM aarch64, ~340K).

### Step 2: Push to Device

```bash
adb push bazel-bin/src/device_agent /data/local/tmp/device_agent
adb shell chmod +x /data/local/tmp/device_agent
```

### Step 3: Run Agent (Socket Mode)

```bash
# Background the agent on device
adb shell '/data/local/tmp/device_agent --socket &'

# Syntax: device_agent --socket [bind_ip] [port]
# Default: bind_ip=0.0.0.0, port=5000
```

### Step 4: Forward Port

```bash
adb forward tcp:5000 tcp:5000
```

### Step 5: Verify

```bash
# Quick smoke test via curl
curl -s -X POST http://127.0.0.1:5000/exec \
  -H "Content-Type: application/json" \
  -d '{"command":"uname -a"}'
```

Expected: `{"exit_code":0,"stdout":"Linux localhost ... aarch64 ...\n","stderr":""}`

### Step 6: Use device_shell Tool

Once forwarding is up, the `device_shell` tool connects automatically to `127.0.0.1:5000`.

## device_agent Modes

| Mode | Flag | Transport | Use Case |
|------|------|-----------|----------|
| Socket | `--socket` | TCP | ADB forward / network |
| PPPoS | *(default)* | Serial + PPP | Direct serial cable |

**Socket mode args:** `[bind_ip] [port]` — both optional, default `0.0.0.0:5000`

**PPPoS mode args:** `[serial] [my_ip] [pc_ip] [port] [baud]` — defaults `/dev/ttyS0 192.168.4.1 192.168.4.2 5000 115200`

## HTTP API

### POST /exec

Execute a shell command on the device.

**Request:**
```json
{
  "command": "shell command string",
  "timeout": 30
}
```

**Response:**
```json
{
  "exit_code": 0,
  "stdout": "command output\n",
  "stderr": ""
}
```

- `timeout` is optional (seconds)

## Troubleshooting

### "Address already in use" on bind
Agent is already running on the device. Kill it first:
```bash
adb shell killall device_agent
```

### "connect to 127.0.0.1:5000 failed"
Forwarding not set up. Re-run:
```bash
adb forward tcp:5000 tcp:5000
```

### Device disappears after sleep
USB may have disconnected. Re-attach and restart agent from Step 3.

### WSL2: ADB can't see device
Windows side needs `usbipd bind` first. See wsl-usb-adb skill.
