#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>

static void kb_keymap(void *d, struct wl_keyboard *kb, uint32_t fmt, int32_t fd,
		uint32_t size) {
	struct stat st; fstat(fd, &st);
	printf("KEYMAP: format=%u fd=%d size=%u fstat_size=%lld\n",
		fmt, fd, size, (long long)st.st_size);
	if (size > 0 && size < (1u << 20)) {
		char *p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (p != MAP_FAILED) {
			printf("  first bytes: %.48s\n", p);
			munmap(p, size);
		}
	}
	fflush(stdout);
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *kb, uint32_t s, struct wl_surface *sf, struct wl_array *a) {
	printf("ENTER serial=%u keys=%zu\n", s, a->size / sizeof(uint32_t)); fflush(stdout); }
static void kb_leave(void *d, struct wl_keyboard *kb, uint32_t s, struct wl_surface *sf) { }
static void kb_key(void *d, struct wl_keyboard *kb, uint32_t s, uint32_t t, uint32_t k, uint32_t st) { }
static void kb_mod(void *d, struct wl_keyboard *kb, uint32_t s, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t g) {
	printf("MODIFIERS serial=%u depressed=%u latched=%u locked=%u group=%u\n", s, d1, d2, d3, g); fflush(stdout); }
static void kb_rep(void *d, struct wl_keyboard *kb, int32_t r, int32_t de) { }
static const struct wl_keyboard_listener kbl = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mod, kb_rep };

static void caps(void *d, struct wl_seat *seat, uint32_t caps) {
	printf("CAPABILITIES 0x%x\n", caps); fflush(stdout);
	if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(kb, &kbl, NULL);
	}
}
static const struct wl_seat_listener sl = { caps, NULL };
static void g(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v) {
	if (!strcmp(i, "wl_seat")) {
		struct wl_seat *s = wl_registry_bind(r, n, &wl_seat_interface, 1);
		wl_seat_add_listener(s, &sl, NULL);
	}
}
static void gr(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener rl = { g, gr };

int main(void) {
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *r = wl_display_get_registry(dpy);
	wl_registry_add_listener(r, &rl, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	for (int i = 0; i < 3; i++) { wl_display_roundtrip(dpy); usleep(200000); }
	printf("done (no keymap => compositor never sent one)\n");
	return 0;
}
