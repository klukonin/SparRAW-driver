#!/bin/sh
# SPDX-License-Identifier: ISC
# Кладёт драйвер и оверлей files/ в дерево OpenWrt.
# Драйвер — форк wil6210 в drivers/net/wireless/ath/wil6210/ этого репозитория;
# в OpenWrt он уходит одним патчем 900-wil6210-sparraw.patch к backports
# из dl/ (тарбол скачивает сам OpenWrt: make package/kernel/mac80211/download).
# Прежние патчи 9xx-wil6210-* из дерева OpenWrt убираются.
# Использование: openwrt/install.sh /путь/к/openwrt
set -e
OWRT=${1:?укажи дерево OpenWrt}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../drivers/net/wireless/ath/wil6210
PD=$OWRT/package/kernel/mac80211/patches/ath
VER=$(sed -n "s/^PKG_SOURCE_VERSION:=//p" "$OWRT/package/kernel/mac80211/Makefile")
BP=$(ls "$OWRT"/dl/backports-"$VER".tar.* 2>/dev/null | head -1)
[ -n "$BP" ] || { echo "нет dl/backports-$VER.tar.*: make package/kernel/mac80211/download"; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
W=drivers/net/wireless/ath/wil6210
tar -xaf "$BP" -C "$T" --wildcards "backports-$VER/$W/*"
mkdir -p "$T/a/${W%/*}" "$T/b/${W%/*}"
mv "$T/backports-$VER/$W" "$T/a/$W"
cp -r "$SRC" "$T/b/$W"
rm -f "$PD"/9*-wil6210-*.patch
{
	echo "wil6210: SparRAW driver (github SparRAW-driver, drivers/$W)"
	echo
	(cd "$T" && diff -urN a b) || [ $? -eq 1 ]
} > "$PD/900-wil6210-sparraw.patch"
mkdir -p "$OWRT/files"
cp -r "$HERE/files/." "$OWRT/files/"
echo "дальше: make package/kernel/mac80211/{clean,compile} V=s"
