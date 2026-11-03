#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Prepare the host so `gn gen` can parse the pruned engine archive.
#
# The container resets between sessions, which drops every apt package but
# keeps /workspace. Re-run this script (it is idempotent) before driving gn.
#
#   sudo scripts/setup_gn_env.sh [engine-root]
#
# It installs the system libraries Chromium's build files probe with
# pkg-config, plus three files the archive omits that GN reads at gen time:
# dri.pc, build/util/LASTCHANGE.committime and the Chromium BRANDING file.
set -u

ENGINE_ROOT="${1:-/workspace/engine/chromium}"
PKG_DIR="/usr/lib/x86_64-linux-gnu/pkgconfig"

if [[ $EUID -ne 0 ]]; then
  echo "This script needs root (it installs packages). Re-run with sudo." >&2
  exit 1
fi

# Every package a BUILD.gn file probes with pkg-config, plus the X11/GTK
# headers the toolchain needs. pkgconf ships the pkg-config binary itself.
PACKAGES=(
  pkgconf
  # TLS / net
  libnss3-dev libnspr4-dev libcurl4-openssl-dev
  # graphics / windowing
  libdrm-dev libgl-dev libegl-dev libgbm-dev
  libglib2.0-dev libdbus-1-dev
  libx11-dev libxext-dev libxdamage-dev libxcomposite-dev libxrandr-dev
  libxtst-dev libxfixes-dev libxi-dev libxrender-dev libxkbcommon-dev
  libxkbcommon-x11-dev
  libcups2-dev libatk1.0-dev libatspi2.0-dev libpango1.0-dev libcairo2-dev
  libexpat1-dev libfontconfig1-dev libfreetype-dev libpixman-1-dev
  libgtk-3-dev libgtk-4-dev libwayland-dev
  # audio / video / codecs
  libasound2-dev libpulse-dev libpipewire-0.3-dev
  libva-dev libopus-dev libvpx-dev libaom-dev libdav1d-dev libflac-dev
  libopenh264-dev libavif-dev
  # misc libraries the media/ui build files look for
  libudev-dev libpci-dev libkrb5-dev liblcms2-dev libpng-dev
  libxml2-dev libxslt1-dev libjsoncpp-dev libre2-dev libbrotli-dev
  libwoff-dev libpcre2-dev libusb-1.0-0-dev libmtdev-dev libevdev-dev
  libinput-dev libgraphite2-dev libicu-dev libharfbuzz-dev
  libepoxy-dev libgraphene-1.0-dev libffi-dev libssl-dev
)
echo "==> installing build dependencies"
apt-get update -qq || true
# Some packages are not in every Debian snapshot; install them one group at a
# time so one missing name does not abort the rest.
for pkg in "${PACKAGES[@]}"; do
  dpkg -s "$pkg" >/dev/null 2>&1 && continue
  apt-get install -y --no-install-recommends "$pkg" >/dev/null 2>&1 \
    || echo "    (skipped unavailable package: $pkg)" >&2
done

# Debian's libdrm-dev does not ship dri.pc, but //build/config/linux/dri reads
# the "dridriverdir" variable from it. Provide a minimal file.
echo "==> writing dri.pc"
cat > "$PKG_DIR/dri.pc" <<'EOF'
prefix=/usr
dridriverdir=/usr/lib/x86_64-linux-gnu/dri

Name: dri
Description: Direct Rendering Infrastructure driver directory
Version: 1.0
EOF

# build/compute_build_timestamp.py reads a unix timestamp from this file; the
# archive carries only LASTCHANGE.dummy.
echo "==> writing LASTCHANGE.committime"
echo 1730000000 > "$ENGINE_ROOT/build/util/LASTCHANGE.committime"

# //build/util/branding.gni shells out to version.py against this file.
echo "==> writing Chromium BRANDING file"
mkdir -p "$ENGINE_ROOT/chrome/app/theme/chromium"
cat > "$ENGINE_ROOT/chrome/app/theme/chromium/BRANDING" <<'EOF'
COMPANY_FULLNAME=The Chromium Authors
COMPANY_SHORTNAME=The Chromium Authors
PRODUCT_FULLNAME=Chromium
PRODUCT_SHORTNAME=Chromium
PRODUCT_INSTALLER_FULLNAME=Chromium Installer
PRODUCT_INSTALLER_SHORTNAME=Chromium Installer
COPYRIGHT=Copyright 2015 The Chromium Authors. All rights reserved.
MAC_BUNDLE_ID=org.chromium.Chromium
MAC_CREATOR_CODE=Cr24
MAC_TEAM_ID=
EOF

echo "==> done"
