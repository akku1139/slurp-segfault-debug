#!/bin/bash
# Build the slurp binaries and the reproducer tools.
#
#   SLURP_SRC=/path/to/slurp repro/build.sh
#
# If SLURP_SRC is not given, the slurp v1.5.0 tarball is fetched with `gh`
# (no need to clone the repository).
#
# Outputs (into ./build):
#   slurp-dbg      slurp 1.5.0 as shipped (crashes)
#   slurp-fixed    same + patches/slurp-null-guard.patch
#   vkctl          virtual-keyboard client used to create the trigger state
#   dumpkb         dumps wl_seat capabilities / wl_keyboard events
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
B=$ROOT/build
mkdir -p "$B"

# ---------- 1. slurp sources ----------
SRC=${SLURP_SRC:-}
if [ -z "$SRC" ]; then
	SRC=$B/slurp-src
	if [ ! -f "$SRC/main.c" ]; then
		echo "== fetching slurp v1.5.0 sources with gh"
		mkdir -p "$SRC.dl"
		gh api repos/emersion/slurp/tarball/v1.5.0 > "$SRC.dl/s.tgz"
		tar xzf "$SRC.dl/s.tgz" -C "$SRC.dl"
		mv "$SRC.dl"/emersion-slurp-* "$SRC"
	fi
fi
[ -f "$SRC/main.c" ] || { echo "no slurp sources at $SRC (set SLURP_SRC)"; exit 1; }
echo "== slurp sources: $SRC"

# ---------- 2. protocol code (no meson needed) ----------
WP=/usr/share/wayland-protocols
P=$B/proto
mkdir -p "$P"
gen() { # $1=xml $2=basename
	wayland-scanner private-code "$1" "$P/$2-protocol.c"
	wayland-scanner client-header "$1" "$P/$2-client-protocol.h"
}
[ -f "$P/.stamp" ] || {
	gen "$WP/stable/xdg-shell/xdg-shell.xml"                 xdg-shell
	gen "$WP/staging/cursor-shape/cursor-shape-v1.xml"       cursor-shape-v1
	gen "$WP/unstable/tablet/tablet-unstable-v2.xml"         tablet-unstable-v2
	gen "$WP/unstable/xdg-output/xdg-output-unstable-v1.xml" xdg-output-unstable-v1
	gen "$SRC/protocol/wlr-layer-shell-unstable-v1.xml"       wlr-layer-shell-unstable-v1
	touch "$P/.stamp"
}

CFLAGS="-O0 -g -Wno-incompatible-pointer-types"
LIBS=$(pkg-config --cflags --libs wayland-client wayland-cursor cairo xkbcommon)

# ---------- 3. the shipped binary ----------
echo "== building slurp-dbg (vulnerable)"
gcc $CFLAGS -o "$B/slurp-dbg" "$SRC"/main.c "$SRC"/pool-buffer.c "$SRC"/render.c \
	"$P"/*.c -I"$SRC/include" -I"$P" $LIBS -lrt

# ---------- 4. the patched binary ----------
echo "== building slurp-fixed (NULL guards)"
rm -rf "$B/slurp-fixed-src"
cp -r "$SRC" "$B/slurp-fixed-src"
( cd "$B/slurp-fixed-src" && patch -p1 <"$ROOT/patches/slurp-null-guard.patch" )
gcc $CFLAGS -o "$B/slurp-fixed" "$B/slurp-fixed-src"/main.c \
	"$B/slurp-fixed-src"/pool-buffer.c "$B/slurp-fixed-src"/render.c \
	"$P"/*.c -I"$B/slurp-fixed-src/include" -I"$P" $LIBS -lrt

# ---------- 5. reproducer tools ----------
echo "== building vkctl + dumpkb"
# vkctl needs generated virtual-keyboard protocol code; generate it locally so we
# do not depend on wayland-protocols shipping the unstable XML.
if [ ! -f "$B/vk.h" ]; then
	wayland-scanner client-header "$ROOT/repro/virtual-keyboard-unstable-v1.xml" "$B/vk.h"
	wayland-scanner private-code  "$ROOT/repro/virtual-keyboard-unstable-v1.xml" "$B/vk-protocol.c"
fi
gcc -O1 -g -o "$B/vkctl" "$ROOT/repro/vkctl.c" "$B/vk-protocol.c" \
	-I"$B" $(pkg-config --cflags --libs wayland-client xkbcommon)
gcc -O1 -g -o "$B/dumpkb" "$ROOT/repro/dumpkb.c" \
	$(pkg-config --cflags --libs wayland-client)

echo "== done: $B/{slurp-dbg,slurp-fixed,vkctl,dumpkb}"
