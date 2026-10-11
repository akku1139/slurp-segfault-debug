#!/bin/bash
# Start a private headless sway and run the A/B matrix for the slurp /
# xkb_state_update_mask() segfault.
#
#   repro/run-repro.sh          # use ./build produced by repro/build.sh
#
# The compositor runs headless (WLR_BACKENDS=headless) in its own
# XDG_RUNTIME_DIR, so it does not touch the running desktop session.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
B=$ROOT/build
RT=${SLURP_REPRO_RUNTIME_DIR:-/tmp/slurp-repro-rt}
export XDG_RUNTIME_DIR=$RT

[ -x "$B/slurp-dbg" ] || { echo "run repro/build.sh first"; exit 1; }

rm -rf "$RT"; mkdir -p "$RT"; chmod 700 "$RT"
cat > "$RT/sway.conf" <<'EOF'
# minimal config: no bars, no background, no keybindings needed
input * {
    xkb_layout us
}
EOF

echo "== starting headless sway in $RT"
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 \
	sway -c "$RT/sway.conf" -d > "$RT/sway.log" 2>&1 &
SWAY_PID=$!
trap 'kill $SWAY_PID 2>/dev/null; pkill -x vkctl 2>/dev/null' EXIT

# The socket name is chosen by wl_display_add_socket_auto(); discover it.
for _ in $(seq 1 100); do
	for s in "$RT"/wayland-*; do
		[ -S "$s" ] && [ "${s%.lock}" = "$s" ] && WAYLAND_DISPLAY=$(basename "$s")
	done
	[ -n "${WAYLAND_DISPLAY:-}" ] && break
	sleep 0.2
done
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
	echo "sway did not come up"; tail -5 "$RT/sway.log"; exit 1
fi
export WAYLAND_DISPLAY
echo "== compositor socket: $WAYLAND_DISPLAY"
sleep 1

claim() { pkill -x vkctl 2>/dev/null; sleep 0.5; }
run() { # $1=binary $2=label $3=vkctl mode
	claim
	setsid nohup "$B/vkctl" "$3" >"$RT/vkctl.log" 2>&1 </dev/null &
	sleep 1.5
	case $3 in transient|keep) sleep 1.5;; esac
	timeout 5 "$1" >"$RT/case.out" 2>&1
	local rc=$?
	case $rc in
		124) echo "  $2: exit=$rc (no crash; still selecting)";;
		139) echo "  $2: exit=$rc (SIGSEGV)";;
		*)   echo "  $2: exit=$rc ($(head -1 "$RT/case.out" 2>/dev/null))";;
	esac
}

echo
echo "== what the compositor tells a fresh client =="
claim
setsid nohup "$B/vkctl" transient >"$RT/vkctl.log" 2>&1 </dev/null &
sleep 3
echo "-- while a transient virtual keyboard has gone away, another one is alive:"
timeout 4 "$B/dumpkb"
echo "-- WAYLAND_DEBUG of the vulnerable slurp in the same state (first wl_keyboard events):"
WAYLAND_DEBUG=1 timeout 4 "$B/slurp-dbg" >"$RT/crash-debug.log" 2>&1
grep -E "wl_seat#.*capabilities|wl_keyboard#" "$RT/crash-debug.log" | head -4 | sed 's/^/   /'
claim

echo
echo "== A/B matrix (3 runs each) =="
for i in 1 2 3; do
	run "$B/slurp-dbg"   "transient vk gone, slurp 1.5.0        run$i" transient
done
for i in 1 2 3; do
	run "$B/slurp-fixed" "transient vk gone, slurp + NULL guard run$i" transient
done
for i in 1 2; do
	run "$B/slurp-dbg"   "control: vk kept alive, slurp 1.5.0  run$i" keep
done
for i in 1 2; do
	run "$B/slurp-dbg"   "control: destroy the inactive vk     run$i" del2
done

echo
echo "== done. logs: $RT/{sway.log,crash-debug.log}"
