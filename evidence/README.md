# Evidence

Raw material behind the write-up in `../README.md`. All of it was produced on
the machine that reported the crash (Arch/CachyOS, sway 1.12 + wlroots 0.20,
slurp 1.5.0, libxkbcommon 1.13.2) using a headless sway on a private
`XDG_RUNTIME_DIR`.

| file | what it is |
|---|---|
| `dmesg-oops.txt` | the reported oops plus the two decoders (`xkb_state_new(NULL)`, `xkb_state_update_mask(NULL,...)`) |
| `gdb-backtrace.txt` | full gdb backtrace of slurp 1.5.0 built with `-O0 -g` in the trigger state |
| `wayland-debug-crash.txt` | `WAYLAND_DEBUG=1` of the crashing run: `capabilities(2)` → `enter` → `modifiers`, **no `keymap`** |
| `wayland-debug-control.txt` | `WAYLAND_DEBUG=1` of the control run (a virtual keyboard kept alive): `keymap` first, no crash |
| `upstream-pr190.patch` | upstream emersion/slurp#190, which adds the NULL guards used as the fix |

Regenerate the Wayland traces with `../repro/run-repro.sh`; it writes
`$XDG_RUNTIME_DIR/crash-debug.log` and `sway.log` under
`SLURP_REPRO_RUNTIME_DIR` (default `/tmp/slurp-repro-rt`).
