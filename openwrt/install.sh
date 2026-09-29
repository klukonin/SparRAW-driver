#!/bin/sh
# SPDX-License-Identifier: ISC
# Кладёт серию 9xx-wil6210 и оверлей files/ в дерево OpenWrt.
# Использование: openwrt/install.sh /путь/к/openwrt
set -e
OWRT=${1:?укажи дерево OpenWrt}
HERE=$(cd "$(dirname "$0")" && pwd)
cp "$HERE"/patches/ath/9*-wil6210-*.patch "$OWRT/package/kernel/mac80211/patches/ath/"
mkdir -p "$OWRT/files"
cp -r "$HERE/files/." "$OWRT/files/"
echo "дальше: make package/kernel/mac80211/{clean,compile} V=s"
