#!/bin/bash
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

AGENT_BIN=./bazel-bin/src/device_agent
CLI_BIN=./bazel-bin/tests/pppos_cli

echo ""
echo "=== device_agent ==="
$AGENT_BIN $P0 192.168.4.1 192.168.4.2 5000 &
AGENT_PID=$!
sleep 2

echo "=== pppos_cli ==="
$CLI_BIN $P1 "echo 'hello from socat test'; uname -a"

echo ""
echo "Press Ctrl+C to stop"
wait $AGENT_PID
