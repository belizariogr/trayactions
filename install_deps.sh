#!/usr/bin/env sh
# Install TrayActions build dependencies (Debian/Ubuntu).

set -eu

PACKAGES="build-essential libgtk-4-dev libjson-c-dev libwayland-dev"

echo "Installing: $PACKAGES"
sudo apt-get update
sudo apt-get install -y $PACKAGES
echo "Dependencies installed. Build with: ./compile.sh"
