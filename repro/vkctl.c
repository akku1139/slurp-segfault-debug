#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "vk.h"

/* Send a real, compilable keymap to a virtual keyboard (required before keys). */
static int send_keymap(struct zwp_virtual_keyboard_v1 *vk) {
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km =
		xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	char *str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t len = strlen(str);
	char *tmp = strdup("/tmp/vkctl-km-XXXXXX");
	int fd = mkstemp(tmp);
	if (write(fd, str, len) != (ssize_t)len) { fprintf(stderr, "short write\n"); }
	lseek(fd, 0, SEEK_SET);
	zwp_virtual_keyboard_v1_keymap(vk, 1 /* XKB_V1 */, fd, (uint32_t)len);
	close(fd);
	free(tmp); free(str); xkb_keymap_unref(km); xkb_context_unref(ctx);
	return 0;
}

struct reg { struct wl_registry *r; struct zwp_virtual_keyboard_manager_v1 *mgr;
	struct wl_seat *seat; };

static void global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t ver) {
	struct reg *g = d;
	if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) && !g->mgr)
		g->mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	else if (!strcmp(iface, "wl_seat") && !g->seat)
		g->seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener rl = { global, global_remove };

int main(int argc, char **argv) {
	const char *mode = argc > 1 ? argv[1] : "add";
	struct reg g = {0};
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { fprintf(stderr, "vkctl: no display\n"); return 1; }
	g.r = wl_display_get_registry(dpy);
	wl_registry_add_listener(g.r, &rl, &g);
	wl_display_roundtrip(dpy);
	if (!g.mgr || !g.seat) { fprintf(stderr, "vkctl: missing globals\n"); return 1; }

	struct zwp_virtual_keyboard_v1 *a =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(g.mgr, g.seat);
	struct zwp_virtual_keyboard_v1 *b = NULL;
	if (!strcmp(mode, "add") || !strcmp(mode, "del2") ||
			!strcmp(mode, "wtype") || !strcmp(mode, "transient"))
		b = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(g.mgr, g.seat);
	wl_display_roundtrip(dpy);
	fprintf(stderr, "vkctl: created a=%p b=%p\n", (void *)a, (void *)b);
	fflush(stderr);

	if (!strcmp(mode, "add") || !strcmp(mode, "del2")) {
		usleep(300 * 1000);
		struct zwp_virtual_keyboard_v1 *victim =
			!strcmp(mode, "del2") ? b : a;
		zwp_virtual_keyboard_v1_destroy(victim);
		fprintf(stderr, "vkctl: destroyed %s\n",
			!strcmp(mode, "del2") ? "b (2nd, non-active)" : "a (1st, active)");
		fflush(stderr);
		wl_display_roundtrip(dpy);
	}
	if (!strcmp(mode, "transient")) {
		/* a := stand-in for the real keyboard (never destroyed)
		 * b := transient virtual keyboard (fcitx5 / wtype style)
		 * b becomes the seat's active keyboard by typing, then goes away.
		 * sway/wlroots then hold keyboard == NULL while still advertising
		 * WL_SEAT_CAPABILITY_KEYBOARD -- so a client that connects in that
		 * window gets wl_keyboard.enter + modifiers with no keymap. */
		send_keymap(a);
		send_keymap(b);
		wl_display_roundtrip(dpy);
		usleep(300 * 1000);
		fprintf(stderr, "vkctl: a types (a becomes active)\n"); fflush(stderr);
		zwp_virtual_keyboard_v1_key(a, 0, 30, 1);
		zwp_virtual_keyboard_v1_key(a, 0, 30, 0);
		wl_display_roundtrip(dpy);
		usleep(300 * 1000);
		fprintf(stderr, "vkctl: b types (b becomes active)\n"); fflush(stderr);
		zwp_virtual_keyboard_v1_key(b, 0, 30, 1);
		zwp_virtual_keyboard_v1_key(b, 0, 30, 0);
		wl_display_roundtrip(dpy);
		usleep(300 * 1000);
		zwp_virtual_keyboard_v1_destroy(b);
		fprintf(stderr, "vkctl: b destroyed; a (real kbd stand-in) stays alive\n");
		fflush(stderr);
		wl_display_roundtrip(dpy);
		usleep(500 * 1000);
		fprintf(stderr, "vkctl: idle (pid %d)\n", getpid()); fflush(stderr);
		while (wl_display_dispatch(dpy) != -1) {}
		return 0;
	}
	if (!strcmp(mode, "wtype2")) {
		/* Exact emulation of "wtype n": a lone virtual keyboard types a key
		 * (which makes sway/wlroots adopt it as the seat's active keyboard)
		 * and then the client exits, destroying it. sway calls
		 * wlr_seat_set_keyboard(NULL) but leaves WL_SEAT_CAPABILITY_KEYBOARD
		 * set, so the next client sees enter+modifiers with no keymap. */
		usleep(300 * 1000);
		zwp_virtual_keyboard_v1_key(a, 0, 30, 1); /* KEY_A press */
		wl_display_roundtrip(dpy);
		usleep(100 * 1000);
		zwp_virtual_keyboard_v1_key(a, 0, 30, 0); /* KEY_A release */
		wl_display_roundtrip(dpy);
		usleep(200 * 1000);
		zwp_virtual_keyboard_v1_destroy(a);
		fprintf(stderr, "vkctl: typed a key, destroyed vk, exiting\n");
		fflush(stderr);
		wl_display_roundtrip(dpy);
		return 0;
	}
	if (!strcmp(mode, "badkeymap")) {
		/* Send a junk keymap through the virtual keyboard: sway forwards it
		 * to clients, slurp will fail to compile it and end up with
		 * xkb_keymap == NULL. */
		char *tmp = strdup("/tmp/vkctl-keymap-XXXXXX");
		int fd = mkstemp(tmp);
		const char *junk = "xkb_keymap { xkb_keycodes { } }; garbage garbage\n";
		write(fd, junk, strlen(junk));
		lseek(fd, 0, SEEK_SET);
		usleep(300 * 1000);
		zwp_virtual_keyboard_v1_keymap(a, 1 /* XKB_V1 */, fd,
			(uint32_t)strlen(junk));
		close(fd);
		fprintf(stderr, "vkctl: sent junk keymap\n"); fflush(stderr);
		wl_display_roundtrip(dpy);
		usleep(500 * 1000);
	}
	if (!strcmp(mode, "esc")) {
		/* sway calls wlr_seat_set_keyboard() on the first key event, which
		 * is what makes it send a keymap to the client. */
		usleep(800 * 1000);
		fprintf(stderr, "vkctl: sending ESC press\n"); fflush(stderr);
		zwp_virtual_keyboard_v1_key(a, 0, 1, 1); /* evdev keycode 1 = ESC */
		wl_display_roundtrip(dpy);
		usleep(200 * 1000);
		zwp_virtual_keyboard_v1_key(a, 0, 1, 0);
		wl_display_roundtrip(dpy);
		fprintf(stderr, "vkctl: sent ESC\n"); fflush(stderr);
		usleep(1500 * 1000);
		return 0;
	}

	fprintf(stderr, "vkctl: idle (pid %d)\n", getpid()); fflush(stderr);
	while (wl_display_dispatch(dpy) != -1) {}
	return 0;
}
