# slurp segfault in `xkb_state_update_mask()` — investigation

`slurp` (region selector for sway) occasionally dies with SIGSEGV inside
`libxkbcommon` when it is launched right after something injected keyboard
input. Example from `dmesg`:

```
[99123.694887] slurp[1252005]: segfault at 0 ip 00007f66b3556c74 sp 00007ffe6bef3850 error 4
              in libxkbcommon.so.0.13.2[3dc74,7f66b351d000+3c000] likely on CPU 6
[99123.694900] Code: ... 48 83 ec 38 <8b> 07 89 44 24 0c ...
```

Context: `grim -g "$(slurp)" - | wl-copy` on sway; seen sporadically, "more
often over Firefox" (Firefox is simply where most screenshots are taken), and
it also happened months ago.

**Short answer:** this is not a xkbcommon bug. `slurp` dereferences a `NULL`
`xkb_state` it never got the chance to create: `sway` advertises
`WL_SEAT_CAPABILITY_KEYBOARD` on a seat whose active keyboard is `NULL` (a
window that opens after a *virtual* keyboard — wtype, fcitx5/IME — goes away
while another keyboard is still attached), so it sends
`wl_keyboard.enter` + `wl_keyboard.modifiers` and **never sends
`wl_keyboard.keymap`**. slurp's very first reaction to that `modifiers` event is
`xkb_state_update_mask(seat->xkb_state /* NULL */, ...)` → SIGSEGV.

Both sides are at fault, but the client-side guard is the one that must exist
(the protocol does not order `keymap` before `modifiers`, see §4). Upstream
already knows: slurp#123, slurp#181, sway#7534, sway#8169, and the open
(unmerged since 2026-02-11) PR emersion/slurp#190 which adds exactly the NULL
guards in `patches/`.

---

## 1. Decoding the crash line

`libxkbcommon.so.0.13.2[3dc74,...]` is an *offset* from the library mapping
base — 0x3dc74, not an address. Resolve it:

```
$ gdb -q -batch -ex "file /usr/lib/libxkbcommon.so.0.13.2" \
      -ex "disassemble /r 0x3dc50,0x3dc90"
   0x000000000003dc50 <xkb_state_update_mask+0>: endbr64
   ...
   0x000000000003dc70 <xkb_state_update_mask+32>: sub    $0x38,%rsp
   0x000000000003dc74 <xkb_state_update_mask+36>: mov    (%rdi),%eax   <-- crash
```

The byte sequence in dmesg (`... 48 83 ec 38 <8b> 07 89 44 24 0c ...`) is the
*same* instruction list, which confirms the decode: `sub $0x38,%rsp` then
`mov (%rdi),%eax`. The `<8b>` marks the faulting byte.

* `ip` = `xkb_state_update_mask + 36`
* fault address `0` with `error 4` (user-mode read of an unmapped page)
* `%rdi` is the first argument = `struct xkb_state *base_state` → **0**

So: `xkb_state_update_mask(NULL, ...)`. Reproduce that trivially:

```c
struct xkb_state *s = xkb_state_new(NULL);   /* already crashes: */
                                             /* xkb_keymap_ref(NULL) -> mov 0x8(%rdi) */
xkb_state_update_mask(NULL, 0,0,0, 0,0,0);   /* exactly our oops */
```

Both crash (see `dmesg` lines for the `xkbtest*` helpers); the second one
matches the reported `ip` exactly. Verified under gdb:

```
0x00007ffff7f59d24 in xkb_keymap_ref () from /usr/lib/libxkbcommon.so.0
#0  xkb_keymap_ref ()          <- xkb_state_new(NULL) does not return NULL, it crashes
#1  xkb_state_new ()
rdi            0x0
```

## 2. Where slurp's `xkb_state` comes from

slurp 1.5.0 (`main.c`) creates the state **only** in the keymap handler:

```c
static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
        const uint32_t format, const int32_t fd, const uint32_t size) {
    ...
    seat->xkb_keymap = xkb_keymap_new_from_buffer(...);   /* may return NULL */
    seat->xkb_state  = xkb_state_new(seat->xkb_keymap);
}

static void keyboard_handle_modifiers(...) {
    xkb_state_update_mask(seat->xkb_state, ...);          /* no NULL check */
}

static void keyboard_handle_key(...) {
    xkb_state_key_get_one_sym(seat->xkb_state, key + 8);  /* no NULL check */
}
```

`struct slurp_seat` is `calloc`'d, so before the keymap event `xkb_state == NULL`.
Per the protocol, `wl_keyboard.keymap` must be sent before `enter`; sway
sometimes does not (see §4), and then the `modifiers` that immediately follows
`enter` is what kills slurp.

## 3. Reproduced locally, deterministically

Environment: headless sway (`WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1`,
sway 1.12 / wlroots 0.20), a private `XDG_RUNTIME_DIR`, and slurp 1.5.0 built
from source (`repro/build.sh`, gcc + wayland-scanner; `O0 -g`).

The reproducer `repro/vkctl.c` uses `zwp_virtual_keyboard_v1` (same protocol
wtype and fcitx5's waylandim frontend use):

1. create virtual keyboard **a** — stands in for the real keyboard, never destroyed
2. create virtual keyboard **b** — the transient one (wtype / IME)
3. `a` types a key → sway/wlroots adopt `a` as `seat->keyboard_state.keyboard`
4. `b` types a key → wlroots *switches* the seat's keyboard to `b`
5. destroy `b` → wlroots runs `wlr_seat_set_keyboard(seat, NULL)` and **never
   sets it again**, while `WL_SEAT_CAPABILITY_KEYBOARD` stays set because `a`
   is still attached to the seat

Then a fresh client sees:

```
CAPABILITIES 0x2                <- KEYBOARD advertised
(no keymap)                     <- repro/dumpkb: not one wl_keyboard.keymap
```

and slurp 1.5.0, on the `modifiers` event that follows `enter`:

```
$ WAYLAND_DEBUG=1 slurp
 -> wl_seat#8.get_keyboard(new id wl_keyboard#13)
    wl_keyboard#13.enter(3, wl_surface#3, array[0])
    wl_keyboard#13.modifiers(4, 0, 0, 0, 0)
zsh: segmentation fault

Program received signal SIGSEGV, Segmentation fault.
0x00007ffff7c8cc74 in xkb_state_update_mask () from /usr/lib/libxkbcommon.so.0
#0  xkb_state_update_mask ()
#1  0x000055555555ad40 in keyboard_handle_modifiers (data=0x555555572470, serial=10,
        mods_depressed=0, mods_latched=0, mods_locked=0, group=0) at main.c:354
        seat = 0x555555572470          <- seat->xkb_state is NULL
#2  libffi.so.8
...
#7  wl_display_dispatch_queue_pending ()
#8  wl_display_roundtrip ()          <- the crash happens during slurp's own startup
```

Note frame #8: the crash lands inside `wl_display_roundtrip()` during init,
never in the selection loop.

### A/B results (same machine, same compositor, 3 runs each)

| scenario | `wl_keyboard.keymap` before `enter` | slurp 1.5.0 | slurp + NULL guard |
|---|---|---|---|
| transient vk destroyed, another vk alive (`vkctl transient`) | **no** | **SIGSEGV ×3** | no crash ×3 |
| only vk alive (`vkctl keep`) | yes | no crash ×2 | no crash |
| two vks, destroy the *active* one (`vkctl add`) | **no** | **SIGSEGV ×3** | — |
| two vks, destroy the inactive one (`vkctl del2`) | yes | no crash | — |

The last two rows are the useful control: it is not "a virtual keyboard
disappeared" that matters, it is *the seat losing its currently-active
keyboard*.

`patches/slurp-null-guard.patch` is the 4-line fix used for the right column
(matches upstream PR #190's guards; #190 additionally moves
`cursor_surface` creation before the second roundtrip to fix the unrelated
NULL-`cursor_surface` crash from slurp#181).

## 4. Why sway sends `modifiers` without a `keymap`

sway#8169 analysed from the wlroots side; confirmed here by reading the sources:

* `wlr_seat_keyboard_enter()` (`wlr/types/seat/wlr_seat_keyboard.c:233`) sends
  `enter` (line 277), sets `focused_client` (line 292) and then calls
  `wlr_seat_keyboard_send_modifiers()` (line 298) — unconditionally, on every
  enter. It never sends a keymap: those only go out from
  `seat_client_create_keyboard()` (line 409) and `wlr_seat_set_keyboard()`
  (lines 154-155) — and `wlr_seat_set_keyboard()` **returns early, sending
  nothing, when `keyboard == NULL`** (line 126). So a seat whose keyboard is
  NULL produces `enter` + `modifiers` and no keymap, forever.
* The seat's keyboard becomes NULL, and stays NULL, when the *currently active*
  keyboard device is destroyed. sway's destroy path is:

  ```
  seat_remove_device()                       sway/input/seat.c:957
    seat_device_destroy()                    sway/input/seat.c:38
      sway_keyboard_destroy()                sway/input/keyboard.c:1129
        if (wlr_seat_get_keyboard(seat) == keyboard->wlr)
            wlr_seat_set_keyboard(wlr_seat, NULL);   keyboard.c:1137-1139
    seat_update_capabilities()               sway/input/seat.c:970
  ```

  `seat_update_capabilities()` then recomputes the capability mask **from the
  devices still attached to the seat** (`sway/input/seat.c:604-626`), so if any
  other keyboard is still attached the mask keeps `WL_SEAT_CAPABILITY_KEYBOARD`
  — exactly the state in §3. Nothing re-runs `wlr_seat_set_keyboard()` with a
  surviving device.
* sway's IME path has the same shape and was partly fixed by sway#8179
  (`sway/input/text_input.c:73` — "ensure keyboard is set before sending
  modifiers"); sway#8169/#7534 are the remaining cases, and
  `input_manager_reset_all_inputs()` (`sway/input/input-manager.c:612`,
  reached from `swaymsg reload`, `sway/config.c:498`) uses the same
  "clear it, then let the next device set it" idiom.
* sway's IME path had the same shape; sway#8179 fixed one instance
  (`sway/input/text_input.c:73` — "ensure keyboard is set before sending
  modifiers"), and sway#8169/#7534 are the remaining cases.

So a client that connects inside that window gets `enter` + `modifiers` for a
seat that advertises a keyboard but has no active keyboard.

Does the protocol allow that? Yes:

* `wl_keyboard.enter` says only *"The compositor must send the
  wl_keyboard.modifiers event after this event"* (`wayland.xml:2619`) — it says
  nothing about a keymap having to come first.
* `wl_keyboard.modifiers` explicitly allows being sent *"without a surface of
  the client having keyboard focus"* (`wayland.xml:2706`) and the interface
  description states that *by default … the active modifiers and the active
  group are 0*, i.e. a client is expected to have a defined modifier state even
  before it is told anything else.
* Only `wl_keyboard.key` is restricted to after `enter`; no event is ordered
  against `keymap`.

So this is a compositor misbehaviour that a client must nonetheless survive:
**the NULL guards belong in slurp** (defensive, and what upstream PR #190 does),
while `wlr_seat_set_keyboard()` in wlroots/sway should also be fixed to restore
a surviving keyboard (sway#8169).

## 5. Why it correlates with Firefox, and why "months ago" too

* Nothing here is Firefox-specific. `slurp` is usually started over Firefox, so
  that's where the sample is.
* The trigger is "a virtual keyboard/IME stopped being the seat's active
  keyboard recently". On this machine the candidates are:
  * **fcitx5** — running in the live session; its `waylandim` v2 frontend
    creates and destroys `zwp_virtual_keyboard_v1` objects. With the default
    `PersistentVirtualKeyboard=false` (`waylandim.h:43`) it resets/recreates
    the virtual keyboard around every activate/deactivate, i.e. exactly the
    transition in §3.
  * **wtype** — the reproducer named in slurp#123/sway#8169.
  * any input tool that briefly occupies the seat's keyboard.
* The window is short (a few ms around the object's destruction), which is why
  it is "sometimes" and not "always", and why it can vanish for months at a
  time.
* Note this is a *different* crash from the one in slurp#181/PR#190's first
  hunk (NULL `cursor_surface` in `pointer_handle_enter`, which needs a
  pointer event during slurp's init and crashes in `wl_proxy_get_version`).
  Both are "input event arrives before slurp finished initialising", which is
  why they are often reported together; the `libxkbcommon` offset in the oops
  distinguishes them.

## 6. Workarounds and fixes

For the reporter, any of:

* patch slurp — `patches/slurp-null-guard.patch` (2 guards, 6 lines), or build
  the upstream PR emersion/slurp#190 branch;
* `slurp` from a build with `xkb_state != NULL` guards (i.e. wait for a 1.5.1
  that includes #190);
* as a workaround with no patching, make sure a real keyboard is the seat's
  active keyboard when slurp starts, e.g. press a key on the physical keyboard
  right before taking the screenshot, or disable fcitx5's Wayland frontend
  (`waylandim`) / avoid `wtype` in the same instant;
* distributor-side: slurp#190 is mergeable but blocked/unmerged since
  2026-02-11 — worth pinging.

## 7. Reproducing it yourself

```
repro/build.sh          # builds slurp (from a checkout) + vkctl + dumpkb
repro/run-repro.sh      # starts a private headless sway and runs the A/B matrix
```

`repro/build.sh` needs a slurp source tree; it expects it in `$SLURP_SRC`
(default `/tmp/slurp-src/emersion-slurp-<sha>` or a `git clone` you point at).
No meson required — it drives `wayland-scanner` and `gcc` directly.

Raw material behind this write-up:

* full gdb backtrace: `evidence/gdb-backtrace.txt`
* `WAYLAND_DEBUG=1` traces (crash vs. control): `evidence/wayland-debug-crash.txt`,
  `evidence/wayland-debug-control.txt`
* `dmesg` oops lines incl. the two decoders: `evidence/dmesg-oops.txt`
