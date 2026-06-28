#!/bin/bash
set -e

SRC="/etc/apt/sources.list.d/ubuntu.sources"

if [ ! -f "$SRC" ]; then
    echo "Error: $SRC not found"
    exit 1
fi

cp "$SRC" "$SRC.bak"
echo "Backup: $SRC.bak"

sed -i 's|http://archive.ubuntu.com/ubuntu/|http://mirrors.aliyun.com/ubuntu/|g' "$SRC"
sed -i 's|http://security.ubuntu.com/ubuntu/|http://mirrors.aliyun.com/ubuntu/|g' "$SRC"

echo "Sources updated. Running apt update..."
apt update
