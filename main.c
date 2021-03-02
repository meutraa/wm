#define _XOPEN_SOURCE 700
#include <X11/Xlib.h>
#include <assert.h>
#include <execinfo.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>

enum { XDGShell, X11Managed, X11Unmanaged };

typedef struct Monitor Monitor;
typedef struct {
  struct wl_list link;
  union {
    struct wlr_xdg_surface *xdg;
    struct wlr_xwayland_surface *xwayland;
  } surface;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener fullscreen;
  struct wl_listener activate;  // xwayland only
  struct wl_listener configure; // xwayland only
  struct wlr_box geom;
  unsigned int type;
  unsigned int tag;
} Client;


typedef struct {
  struct wl_list link;
  struct wlr_input_device *device;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
} Input;

struct render_data {
  struct timespec *when;
  int x, y; // layout-relative
};

static struct wl_list clients; // tiling order
static struct wl_list independents;
static struct wlr_xcursor_manager *cm;

static struct wlr_renderer *renderer;
static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xwayland *xwayland;
static struct wlr_output_layout *ol;
static struct wlr_cursor *cursor;
static struct wlr_seat *seat;

// Output related things
struct wlr_output *mo;
struct wl_listener mon_frame;
struct wl_listener mon_destroy;
static int mh = 1440;
static int mw = 5120;
static int tag;

static Client *sclient;
static Client *fsclient;

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define LENGTH(X) (sizeof X / sizeof X[0])
#define END(A) ((A) + LENGTH(A))
#define TAGMASK ((1 << LENGTH(tags)) - 1)
#define log(fmt, ...) wlr_log(WLR_INFO, fmt, ##__VA_ARGS__)
#define panic(fmt, ...)                                                        \
  wlr_log(WLR_ERROR, fmt, ##__VA_ARGS__);                                      \
  exit(EXIT_FAILURE);

#define for_each(T, L)                                                         \
  T *it = NULL;                                                                \
  wl_list_for_each(it, &L, link)

#define for_each_reverse(T, L)                                                 \
  T *it = NULL;                                                                \
  wl_list_for_each_reverse(it, &L, link)

#define CASE(K, C)                                                             \
  case K:                                                                      \
    C;                                                                         \
    return 1

Client *xytoclient(double x, double y) {
  for_each(Client, clients) {
    if (it->tag == tag && wlr_box_contains_point(&it->geom, x, y)) {
      return it;
    }
  }
  return NULL;
}

Client *xytoindependent(double x, double y) {
  for_each_reverse(Client, independents) {
    if (wlr_box_contains_point(
            &(struct wlr_box){
                .x = it->surface.xwayland->x,
                .y = it->surface.xwayland->y,
                .width = it->surface.xwayland->width,
                .height = it->surface.xwayland->height,
            },
            x, y)) {
      return it;
    }
  }
  return NULL;
}

static inline void client_activate_surface(struct wlr_surface *s,
                                           int activated) {
  if (wlr_surface_is_xwayland_surface(s)) {
    wlr_xwayland_surface_activate(wlr_xwayland_surface_from_wlr_surface(s),
                                  activated);
    return;
  }
  if (wlr_surface_is_xdg_surface(s)) {
    struct wlr_xdg_surface *sur = wlr_xdg_surface_from_wlr_surface(s);
    if (NULL != sur) {
      wlr_xdg_toplevel_set_activated(sur, activated);
    };
  };
}

static inline const char *client_get_appid(Client *c) {
  return c->type == XDGShell ? c->surface.xdg->toplevel->app_id
                             : c->surface.xwayland->class;
}

static inline struct wlr_surface *client_surface(Client *c) {
  return c->type == XDGShell ? c->surface.xdg->surface
                             : c->surface.xwayland->surface;
}

static inline struct wlr_surface *
client_surface_at(Client *c, double cx, double cy, double *sx, double *sy) {
  return c->type == XDGShell
             ? wlr_xdg_surface_surface_at(c->surface.xdg, cx, cy, sx, sy)
             : wlr_surface_surface_at(c->surface.xwayland->surface, cx, cy, sx,
                                      sy);
}

void set_geometry(Client *c, int x, int y, int w, int h) {
  c->geom = (struct wlr_box){.x = x, .y = y, .width = w, .height = h};
  if (c->type == XDGShell) {
    wlr_xdg_toplevel_set_size(c->surface.xdg, w, h);
    return;
  }
  wlr_xwayland_surface_configure(c->surface.xwayland, x, y, w, h);
}

int isfloating(Client *c) {
  const char *title = client_get_appid(c);
  return NULL != title &&
         (strcmp(title, "floating") == 0 || strcmp(title, "gcr-prompter") == 0);
}

void arrange() {
  log("%s", "arranging");
  unsigned int i = 0, n = 0, cols = 0, rows = 0, cn = 0, rn = 0, cx = 0, cy = 0, cw = 0, ch = 0;
  Client *c;

  wl_list_for_each(c, &clients, link) {
    if (c->tag == tag && !isfloating(c)) {
      n++;
    }
  }

  if (n != 0) {
	  if (n == 1) cols = 1;
	  else if (n == 2) cols = 2;
	  else if (n <= 6) cols = 3;
	  else cols = 4;

	  rows = n/cols;

	  cw = cols ? mw / cols : mw;
	}

  for_each(Client, clients) {
    if (it->tag != tag) {
      log("%s", "it is not on this tag");
      continue;
    }

    if (fsclient == it) {
      log("%s", "it is fullscreen");
      set_geometry(it, 0, 0, mw, mh);
      break;
    }

    if (isfloating(it)) {
      log("%s", "it is floating!");
      set_geometry(it, 0, 0, 640, 480);
      continue;
    }

		if(i/rows + 1 > cols - n%cols) {
			rows = n/cols + 1;
    }
		ch = rows ? mh/ rows : mh;
		cx = cn*cw;
		cy = rn*ch;
    log("%d.x %d.y %d.w %d.h", cx, cy, cw, ch);
		set_geometry(it, cx, cy, cw, ch);
		rn++;
		if(rn >= rows) {
			rn = 0;
			cn++;
		}
    i++;
  }
}

void focus(Client *c) {
  struct wlr_surface *old = seat->keyboard_state.focused_surface;
  if (c && client_surface(c) == old) {
    return;
  }

  sclient = c;
  if (old && (!c || client_surface(c) != old)) {
    client_activate_surface(old, 0);
  }

  if (!c) {
    wlr_seat_keyboard_notify_clear_focus(seat);
    return;
  }

  struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
  wlr_seat_keyboard_notify_enter(seat, client_surface(c), kb->keycodes,
                                 kb->num_keycodes, &kb->modifiers);

  client_activate_surface(client_surface(c), 1);
}

void on_cursor_axis(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_axis *e = data;
  wlr_seat_pointer_notify_axis(seat, e->time_msec, e->orientation, e->delta,
                               e->delta_discrete, e->source);
}

void on_cursor_button(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_button *e = data;
  wlr_seat_pointer_notify_button(seat, e->time_msec, e->button, e->state);
}

void on_output_destroy(struct wl_listener *listener, void *data) {
  log("%s", "on_output_destroy");
  wlr_output_layout_remove(ol, mo);
  wl_list_remove(&mon_destroy.link);
  wl_list_remove(&mon_frame.link);
  mo = NULL;
}

void render(struct wlr_surface *surface, int sx, int sy, void *data) {
  struct render_data *rdata = data;
  double ox = 0, oy = 0;

  struct wlr_texture *texture = wlr_surface_get_texture(surface);
  if (texture) {
    wlr_render_texture(renderer, texture, mo->transform_matrix,
                       ox + rdata->x + sx, oy + rdata->y + sy, 1.0);
    wlr_surface_send_frame_done(surface, rdata->when);
  }
}

void on_output_frame(struct wl_listener *listener, void *data) {
  if (!wlr_output_attach_render(mo, NULL)) {
    return;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  wlr_renderer_begin(renderer, mw, mh);
  wlr_renderer_clear(renderer, (float[]){0.0, 0.0, 0.0, 1.0});

  Client *it = NULL;
  wl_list_for_each_reverse(it, &clients, link) {
    if (it->tag != tag) {
      continue;
    }

    struct render_data *d = &(struct render_data){
        .when = &now,
        .x = it->geom.x,
        .y = it->geom.y,
    };

    if (it->type == XDGShell) {
      wlr_xdg_surface_for_each_surface(it->surface.xdg, render, d);
    } else {
      wlr_surface_for_each_surface(it->surface.xwayland->surface, render, d);
    }
  }

  Client *in;
  wl_list_for_each(in, &independents, link) {
      wlr_surface_for_each_surface(in->surface.xwayland->surface, render,
                                   &(struct render_data){
                                       .when = &now,
                                       .x = in->surface.xwayland->x,
                                       .y = in->surface.xwayland->y,
                                   });
  }

  wlr_renderer_end(renderer);
  wlr_output_commit(mo);
}

void on_backend_new_output(struct wl_listener *listener, void *data) {
  log("%s", "on_backend_new_output");
  mo = data;
  mon_frame.notify = on_output_frame;
  mon_destroy.notify = on_output_destroy;

  for_each(struct wlr_output_mode, mo->modes) {
    log("%dx%d@%d", it->width, it->height, it->refresh);
    if (it->width == 5120 && it->height == 1440 && it->refresh == 239761) {
      wlr_output_set_mode(mo, it);
      break;
    }
  }
  wlr_output_enable_adaptive_sync(mo, 1);

  wl_signal_add(&mo->events.frame, &mon_frame);
  wl_signal_add(&mo->events.destroy, &mon_destroy);
  wlr_output_layout_add_auto(ol, mo);

  wlr_xcursor_manager_load(cm, 1);
  wlr_xcursor_manager_set_cursor_image(cm, "left_ptr", cursor);

  wlr_output_enable(mo, 1);
  if (wlr_output_commit(mo)) {
    arrange();
  }
}

// Ready to manage this surface
void on_xdg_surface_map(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_map");
  Client *c = wl_container_of(listener, c, map);
  if (c->type == X11Unmanaged) {
    wl_list_insert(&independents, &c->link);
    return;
  }
  wl_list_insert(&clients, &c->link);
  arrange();
  focus(c);
}

// Stop managing this surface
void on_xdg_surface_unmap(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_unmap");
  Client *c = wl_container_of(listener, c, unmap);
  int sel = sclient == c;
  wl_list_remove(&c->link);
  arrange();
  if (sel) {
    focus(xytoclient(cursor->x, cursor->y));
  }
}

void on_xdg_surface_destroy(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_destroy");
  Client *c = wl_container_of(listener, c, destroy);
  wl_list_remove(&c->map.link);
  wl_list_remove(&c->unmap.link);
  wl_list_remove(&c->destroy.link);
  if (c->type == X11Managed) {
    wl_list_remove(&c->activate.link);
  } else if (c->type == XDGShell) {
    wl_list_remove(&c->fullscreen.link);
  }
  free(c);
}

void on_xdg_surface_fullscreen(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_fullscreen");
  Client *c = wl_container_of(listener, c, fullscreen);
  fsclient = fsclient ? NULL : c;
  wlr_xdg_toplevel_set_fullscreen(c->surface.xdg, fsclient);
  arrange();
}

void on_xdg_new_surface(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_new_surface");
  struct wlr_xdg_surface *s = data;

  if (s->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    Client *c = s->data = calloc(1, sizeof(*c));
    c->surface.xdg = s;
    c->type = XDGShell;
    c->map.notify = on_xdg_surface_map;
    c->unmap.notify = on_xdg_surface_unmap;
    c->destroy.notify = on_xdg_surface_destroy;
    c->fullscreen.notify = on_xdg_surface_fullscreen;

    wlr_xdg_toplevel_set_tiled(c->surface.xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
                                                   WLR_EDGE_LEFT |
                                                   WLR_EDGE_RIGHT);

    wl_signal_add(&s->events.map, &c->map);
    wl_signal_add(&s->events.unmap, &c->unmap);
    wl_signal_add(&s->events.destroy, &c->destroy);
    wl_signal_add(&s->toplevel->events.request_fullscreen, &c->fullscreen);
  }
}

void on_cursor_frame(struct wl_listener *listener, void *data) {
  wlr_seat_pointer_notify_frame(seat);
}

void focusstack(const int dir) {
  if (NULL == sclient) {
    return;
  }

  Client *c;
  if (dir > 0) {
    wl_list_for_each(c, &sclient->link, link) {
      if (c->tag == tag) break;
    }
  } else {
    wl_list_for_each_reverse(c, &sclient->link, link) {
      if (c->tag == tag) break;
    }
  }
  if (c) {
    focus(c);
  }
}

void sigchld(int unused) {
  if (signal(SIGCHLD, sigchld) == SIG_ERR) {
    log("%s", "can't install SIGCHLD handler");
  }
  while (0 < waitpid(-1, NULL, WNOHANG))
    ;
}

void spawn(const char *cmd) {
  if (fork() == 0) {
    setsid();
    execvp(cmd, (char *[]){NULL});
  }
}

void select() {
  if (sclient) {
    wl_list_remove(&sclient->link);
    wl_list_insert(&clients, &sclient->link);
  }
}

void tagit(const int tag) {
  if (!sclient || sclient->tag == tag) {
    return;
  }
  sclient->tag = tag;
  arrange();
}

void view(const int t) {
  if (tag == t) {
    return;
  }
  tag = t;
  arrange();
}

void kill_client() {
  if (!sclient) {
    return;
  }
  if (sclient->type == XDGShell) {
    wlr_xdg_toplevel_send_close(sclient->surface.xdg);
  } else {
    wlr_xwayland_surface_close(sclient->surface.xwayland);
  }
}

int handle_key(uint32_t code, uint32_t mods) {
  if (mods == WLR_MODIFIER_LOGO) {
    switch (code) {
      CASE(28, spawn("launcher"));
      CASE(25, spawn("passmenu"));
      CASE(57, select());
      CASE(46, focusstack(1));
      CASE(35, focusstack(-1));
      CASE(23, view(0));
      CASE(18, view(1));
      CASE(24, view(2));
      CASE(49, view(3));
    }
  } else if (mods == (WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL)) {
    switch (code) {
      CASE(46, kill_client());
      CASE(28, spawn("alacritty"));
      CASE(23, tagit(0));
      CASE(18, tagit(1));
      CASE(24, tagit(2));
      CASE(49, tagit(3));
    }
  }
  return 0;
}

void on_keyboard_key(struct wl_listener *listener, void *data) {
  Input *input = wl_container_of(listener, input, key);
  struct wlr_event_keyboard_key *e = data;
  uint32_t mods = wlr_keyboard_get_modifiers(input->device->keyboard);
  if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
      handle_key(e->keycode, mods)) {
    return;
  }

  wlr_seat_set_keyboard(seat, input->device);
  wlr_seat_keyboard_notify_key(seat, e->time_msec, e->keycode, e->state);
}

void on_keyboard_modifiers(struct wl_listener *listener, void *data) {
  Input *input = wl_container_of(listener, input, modifiers);
  wlr_seat_set_keyboard(seat, input->device);
  wlr_seat_keyboard_notify_modifiers(seat, &input->device->keyboard->modifiers);
}

void on_input_destroy(struct wl_listener *listener, void *data) {
  log("%s", "on_input_destroy");
  struct wlr_input_device *device = data;
  Input *input = device->data;
  wl_list_remove(&input->modifiers.link);
  wl_list_remove(&input->key.link);
  wl_list_remove(&input->destroy.link);
  free(input);
}

void on_backend_new_input(struct wl_listener *listener, void *data) {
  struct wlr_input_device *device = data;
  log("on_backend_new_input: (%d): %s", device->type, device->name);

  if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
    struct xkb_context *context;
    struct xkb_keymap *keymap;

    Input *input = device->data = calloc(1, sizeof(*input));
    input->device = device;

    context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    keymap = xkb_map_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(device->keyboard, keymap);
    wlr_keyboard_set_repeat_info(device->keyboard, 25, 220);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    input->key.notify = on_keyboard_key;
    input->destroy.notify = on_input_destroy;
    input->modifiers.notify = on_keyboard_modifiers;

    wl_signal_add(&device->keyboard->events.modifiers, &input->modifiers);
    wl_signal_add(&device->keyboard->events.key, &input->key);
    wl_signal_add(&device->events.destroy, &input->destroy);

    wlr_seat_set_keyboard(seat, device);
  } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
    wlr_cursor_attach_input_device(cursor, device);
  }
}

void on_cursor_motion(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_motion *e = data;
  wlr_cursor_move(cursor, e->device, e->delta_x, e->delta_y);

  double sx = 0, sy = 0;
  struct wlr_surface *surface = NULL;
  Client *c = NULL;

  if ((c = xytoindependent(cursor->x, cursor->y))) {
    surface = wlr_surface_surface_at(
        c->surface.xwayland->surface, cursor->x - c->surface.xwayland->x,
        cursor->y - c->surface.xwayland->y, &sx, &sy);
  } else if ((c = xytoclient(cursor->x, cursor->y))) {
    surface = client_surface_at(c, cursor->x - c->geom.x, cursor->y - c->geom.y, &sx, &sy);
  }

  if (c && !surface) {
    surface = client_surface(c);
  }

  if (!surface) {
    wlr_seat_pointer_notify_clear_focus(seat);
    return;
  }

  if (surface == seat->pointer_state.focused_surface) {
    wlr_seat_pointer_notify_motion(seat, e->time_msec, sx, sy);
    return;
  }

  wlr_seat_pointer_notify_enter(seat, surface, sx, sy);

  if (c && c->type != X11Unmanaged) {
    focus(c);
  }
}

void on_seat_request_set_cursor(struct wl_listener *listener, void *data) {
  log("%s", "on_seat_request_cursor");
  struct wlr_seat_pointer_request_set_cursor_event *e = data;
  if (e->seat_client == seat->pointer_state.focused_client) {
    wlr_cursor_set_surface(cursor, e->surface, e->hotspot_x, e->hotspot_y);
  }
}

void on_seat_request_set_primary_selection(struct wl_listener *listener,
                                           void *data) {
  log("%s", "on_seat_set_primary_selection");
  struct wlr_seat_request_set_primary_selection_event *e = data;
  wlr_seat_set_primary_selection(seat, e->source, e->serial);
}

void on_seat_request_set_selection(struct wl_listener *listener, void *data) {
  log("%s", "on_seat_request_set_selection");
  struct wlr_seat_request_set_selection_event *e = data;
  wlr_seat_set_selection(seat, e->source, e->serial);
}

void on_xwayland_surface_request_activate(struct wl_listener *listener,
                                          void *data) {
  log("%s", "on_xwayland_surface_request_activate");
  Client *c = wl_container_of(listener, c, activate);
  if (c->type == X11Managed) {
    wlr_xwayland_surface_activate(c->surface.xwayland, 1);
  }
}

void on_xwayland_surface_request_configure(struct wl_listener *listener,
                                           void *data) {
  log("%s", "on_xwayland_surface_request_configure");
  Client *c = wl_container_of(listener, c, configure);
  struct wlr_xwayland_surface_configure_event *e = data;
  wlr_xwayland_surface_configure(c->surface.xwayland, e->x, e->y, e->width,
                                 e->height);
}

void on_xwayland_new_surface(struct wl_listener *listener, void *data) {
  log("%s", "on_xwayland_new_surface");
  struct wlr_xwayland_surface *xwayland_surface = data;

  Client *c = calloc(1, sizeof(Client));
  c->surface.xwayland = xwayland_surface;
  c->type = xwayland_surface->override_redirect ? X11Unmanaged : X11Managed;
  c->map.notify = on_xdg_surface_map;
  c->unmap.notify = on_xdg_surface_unmap;
  c->activate.notify = on_xwayland_surface_request_activate;
  c->configure.notify = on_xwayland_surface_request_configure;
  c->destroy.notify = on_xdg_surface_destroy;

  wl_signal_add(&xwayland_surface->events.map, &c->map);
  wl_signal_add(&xwayland_surface->events.unmap, &c->unmap);
  wl_signal_add(&xwayland_surface->events.request_activate, &c->activate);
  wl_signal_add(&xwayland_surface->events.request_configure, &c->configure);
  wl_signal_add(&xwayland_surface->events.destroy, &c->destroy);
}

void on_xwayland_ready(struct wl_listener *listener, void *data) {
  log("%s", "on_xwayland_ready");
  xcb_connection_t *xc = xcb_connect(xwayland->display_name, NULL);
  if (xcb_connection_has_error(xc)) {
    return;
  }

  wlr_xwayland_set_seat(xwayland, seat);
  xcb_disconnect(xc);
}

void handler(int sig) {
  void *array[10];
  size_t size;

  size = backtrace(array, 10);

  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}

int main(int argc, char *argv[]) {
  wlr_log_init(WLR_INFO, NULL);
  assert(getenv("XDG_RUNTIME_DIR"));

  signal(SIGSEGV, handler);
  sigchld(0);

  wl_list_init(&clients);
  wl_list_init(&independents);

  struct wl_display *display = wl_display_create();
  struct wlr_backend *backend = wlr_backend_autocreate(display);
  assert(backend);

  renderer = wlr_backend_get_renderer(backend);
  assert(wlr_renderer_init_wl_display(renderer, display));
  struct wlr_compositor *compositor = wlr_compositor_create(display, renderer);
  cm = wlr_xcursor_manager_create(NULL, 36);
  xdg_shell = wlr_xdg_shell_create(display);
  cursor = wlr_cursor_create();
  ol = wlr_output_layout_create();
  seat = wlr_seat_create(display, "seat0");
  assert(xwayland = wlr_xwayland_create(display, compositor, 1));

  wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER |
                                      WL_SEAT_CAPABILITY_KEYBOARD);
  wlr_xcursor_manager_set_cursor_image(cm, "left_ptr", cursor);
	wlr_cursor_attach_output_layout(cursor, ol);
  wlr_export_dmabuf_manager_v1_create(display);
  wlr_data_control_manager_v1_create(display);
  wlr_data_device_manager_create(display);
  wlr_primary_selection_v1_device_manager_create(display);
  wlr_viewporter_create(display);

  // Register listeners
  wl_signal_add(&backend->events.new_output,
                &(struct wl_listener){.notify = on_backend_new_output});
  wl_signal_add(&xdg_shell->events.new_surface,
                &(struct wl_listener){.notify = on_xdg_new_surface});
  wl_signal_add(&cursor->events.motion,
                &(struct wl_listener){.notify = on_cursor_motion});
  wl_signal_add(&cursor->events.button,
                &(struct wl_listener){.notify = on_cursor_button});
  wl_signal_add(&cursor->events.axis,
                &(struct wl_listener){.notify = on_cursor_axis});
  wl_signal_add(&cursor->events.frame,
                &(struct wl_listener){.notify = on_cursor_frame});
  wl_signal_add(&backend->events.new_input,
                &(struct wl_listener){.notify = on_backend_new_input});
  wl_signal_add(&seat->events.request_set_selection,
                &(struct wl_listener){.notify = on_seat_request_set_selection});
  wl_signal_add(&seat->events.request_set_cursor,
                &(struct wl_listener){.notify = on_seat_request_set_cursor});
  wl_signal_add(
      &seat->events.request_set_primary_selection,
      &(struct wl_listener){.notify = on_seat_request_set_primary_selection});
  wl_signal_add(&xwayland->events.ready,
                &(struct wl_listener){.notify = on_xwayland_ready});
  wl_signal_add(&xwayland->events.new_surface,
                &(struct wl_listener){.notify = on_xwayland_new_surface});

  const char *socket = wl_display_add_socket_auto(display);
  assert(socket);

  setenv("DISPLAY", xwayland->display_name, 1);
  setenv("WAYLAND_DISPLAY", socket, 1);

  assert(wlr_backend_start(backend));

  wl_display_run(display);

  wlr_xwayland_destroy(xwayland);
  wl_display_destroy_clients(display);
  wlr_backend_destroy(backend);
  wlr_cursor_destroy(cursor);
  wlr_seat_destroy(seat);
  wl_display_destroy(display);
  return EXIT_SUCCESS;
}
