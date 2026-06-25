#!/bin/bash
# 本地模拟: socat 创建 pty 对，一边跑 device_agent，另一边跑 edgeclaw
set -e

P0=/tmp/lwip_dev_pty
P1=/tmp/lwip_pc_pty
rm -f $P0 $P1

socat PTY,link=$P0,rawer,echo=0 PTY,link=$P1,rawer,echo=0 &
SOCAT_PID=$!
sleep 0.5
echo "pty pair: $P0 <-> $P1"

cleanup() { kill $SOCAT_PID 2>/dev/null; rm -f $P0 $P1; }
trap cleanup EXIT

echo ""
echo "=== Terminal 1: device agent ==="
bazel run //src:device_agent -- $P0 192.168.4.1 192.168.4.2 5000 &
AGENT_PID=$!

echo ""
echo "=== Terminal 2: edgeclaw ==="
echo "(uses config.json with \"device_pty\":\"$P1\")"
echo ""
echo "Press Ctrl+C to stop both"

wait $AGENT_PID
