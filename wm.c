#define _XOPEN_SOURCE 700
#include <X11/Xlib.h>
#include <assert.h>
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
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
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
  struct wlr_box geom;          // layout-relative
  Monitor *mon;
  unsigned int type;
  unsigned int tags;
} Client;

struct Monitor {
  struct wl_list link;
  struct wlr_output *wlr_output;
  struct wl_listener frame;
  struct wl_listener destroy;
  struct wlr_box m; /* monitor area, layout-relative */
  struct wlr_box w; /* window area, layout-relative */
  unsigned int seltags;
  unsigned int tagset[2];
  int position;
  Client *fullscreen;
};

typedef struct {
  struct wl_list link;
  struct wlr_input_device *device;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
} Input;

struct render_data {
  struct wlr_output *output;
  struct timespec *when;
  int x, y; // layout-relative
  int focused;
};

static struct wl_list clients; // tiling order
static struct wl_list independents;
static struct wl_list mons;

static struct wlr_renderer *renderer;
static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xwayland *xwayland;
static struct wlr_cursor *cursor;
static struct wlr_seat *seat;
static struct wlr_output_layout *output_layout;

static Client *dclient;
static Client *sclient;
static int grabcx, grabcy;

static struct wlr_box sgeom;

static Monitor *smon;

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
    return C

static const char *tags[] = {"i", "e", "o", "n"};

static inline int visibleon(Client *c, Monitor *m) {
  return c->mon == m && (c->tags & m->tagset[m->seltags]);
}

Client *xytoclient(double x, double y) {
  for_each(Client, clients) {
    if (visibleon(it, it->mon) && wlr_box_contains_point(&it->geom, x, y)) {
      return it;
    }
  }
  return NULL;
}

Monitor *xytomon(double x, double y) {
  struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
  return o ? o->data : NULL;
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
  if (wlr_surface_is_xdg_surface(s))
    wlr_xdg_toplevel_set_activated(wlr_xdg_surface_from_wlr_surface(s),
                                   activated);
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

void arrange(Monitor *m) {
  Client *c;
  int n = 0;
  wl_list_for_each(c, &clients, link) {
    if (visibleon(c, m) && !isfloating(c)) {
      n++;
    }
  }

  int i = 0;
  for_each_reverse(Client, clients) {
    if (!visibleon(it, m)) {
      continue;
    }

    if (it->mon->fullscreen == it) {
      set_geometry(it, m->m.x, m->m.y, m->m.width, m->m.height);
      break;
    }

    if (isfloating(it)) {
      // TODO, just center it.
      set_geometry(it, m->w.x + 640, 360, 640, 360);
      continue;
    }

    int sidewidth = m->m.width / n;
    sidewidth = sidewidth == m->m.width ? 0 : sidewidth;
    int mainwidth = m->m.width - sidewidth;
    if (i == 0) {
      set_geometry(it, m->m.x, m->m.y, mainwidth, m->m.height);
    } else {
      int sideheight = m->m.height / (n - 1);
      int sidey = sideheight * (i - 1);
      set_geometry(it, m->m.x + mainwidth, sidey, sidewidth, sideheight);
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
  smon = c ? c->mon : smon;

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

void setmon(Client *c, Monitor *m, unsigned int newtags) {
  Monitor *oldmon = c->mon;

  c->mon = m;
  int isfullscreen = 0;

  if (oldmon) {
    wlr_surface_send_leave(client_surface(c), oldmon->wlr_output);
    if (oldmon->fullscreen == c) {
      isfullscreen = 1;
      oldmon->fullscreen = NULL;
    }
    arrange(oldmon);
  }
  if (m) {
    wlr_surface_send_enter(client_surface(c), m->wlr_output);
    c->tags = newtags ? newtags : m->tagset[m->seltags];
    if (isfullscreen) {
      if (m->fullscreen) {
        wlr_xdg_toplevel_set_fullscreen(m->fullscreen->surface.xdg, 0);
      }
      m->fullscreen = c;
    }
    arrange(m);
  }
}

void updatemons() {
  sgeom = *wlr_output_layout_get_box(output_layout, NULL);
  for_each(Monitor, mons) {
    it->m = it->w = *wlr_output_layout_get_box(output_layout, it->wlr_output);
    arrange(it);
  }
}

void on_cursor_axis(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_axis *e = data;
  wlr_seat_pointer_notify_axis(seat, e->time_msec, e->orientation, e->delta,
                               e->delta_discrete, e->source);
}

void on_cursor_button(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_button *e = data;

  if (e->state == WLR_BUTTON_PRESSED && e->button == BTN_SIDE) {
    dclient = xytoclient(cursor->x, cursor->y);
    if (dclient) {
      grabcx = cursor->x - dclient->geom.x;
      grabcy = cursor->y - dclient->geom.y;
    }
    return;
  } else if (WLR_BUTTON_RELEASED == e->state && NULL != dclient) {
    smon = xytomon(cursor->x, cursor->y);
    setmon(dclient, smon, 0);
    dclient = NULL;
    return;
  }

  wlr_seat_pointer_notify_button(seat, e->time_msec, e->button, e->state);
}

void on_output_destroy(struct wl_listener *listener, void *data) {
  log("%s", "on_output_destroy");
  struct wlr_output *wlr_output = data;
  Monitor *m = wlr_output->data;
  int nmons, i = 0;

  wl_list_remove(&m->destroy.link);
  wl_list_remove(&m->frame.link);
  wl_list_remove(&m->link);
  wlr_output_layout_remove(output_layout, m->wlr_output);
  updatemons();

  nmons = wl_list_length(&mons);
  do
    smon = wl_container_of(mons.prev, smon, link);
  while (!smon->wlr_output->enabled && i++ < nmons);
  for_each(Client, clients) {
    if (it->mon == m) {
      setmon(it, smon, it->tags);
    }
  }
  free(m);
}

void render(struct wlr_surface *surface, int sx, int sy, void *data) {
  struct render_data *rdata = data;
  struct wlr_output *output = rdata->output;
  double ox = 0, oy = 0;

  struct wlr_texture *texture = wlr_surface_get_texture(surface);
  if (texture) {
    wlr_output_layout_output_coords(output_layout, output, &ox, &oy);
    wlr_render_texture(renderer, texture, output->transform_matrix,
                       ox + rdata->x + sx, oy + rdata->y + sy,
                       rdata->focused ? 1 : 0.8);
    wlr_surface_send_frame_done(surface, rdata->when);
  }
}

void on_output_frame(struct wl_listener *listener, void *data) {
  Monitor *m = wl_container_of(listener, m, frame);

  if (!wlr_output_attach_render(m->wlr_output, NULL)) {
    return;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  wlr_renderer_begin(renderer, m->wlr_output->width, m->wlr_output->height);
  wlr_renderer_clear(renderer, (float[]){0.0, 0.0, 0.0, 1.0});

  Client *it = NULL;
  wl_list_for_each_reverse(it, &clients, link) {
    if (!visibleon(it, it->mon) ||
        !wlr_output_layout_intersects(output_layout, m->wlr_output,
                                      &it->geom)) {
      continue;
    }

    double ox = it->geom.x;
    double oy = it->geom.y;
    wlr_output_layout_output_coords(output_layout, m->wlr_output, &ox, &oy);

    struct render_data *d = &(struct render_data){
        .output = m->wlr_output,
        .when = &now,
        .x = it->geom.x,
        .y = it->geom.y,
        .focused = it == sclient,
    };

    if (it->type == XDGShell) {
      wlr_xdg_surface_for_each_surface(it->surface.xdg, render, d);
    } else {
      wlr_surface_for_each_surface(it->surface.xwayland->surface, render, d);
    }
  }

  Client *in;
  wl_list_for_each(in, &independents, link) {
    if (wlr_output_layout_intersects(output_layout, m->wlr_output,
                                     &(struct wlr_box){
                                         .x = in->surface.xwayland->x,
                                         .y = in->surface.xwayland->y,
                                         .width = in->surface.xwayland->width,
                                         .height = in->surface.xwayland->height,
                                     })) {
      wlr_surface_for_each_surface(in->surface.xwayland->surface, render,
                                   &(struct render_data){
                                       .output = m->wlr_output,
                                       .when = &now,
                                       .x = in->surface.xwayland->x,
                                       .y = in->surface.xwayland->y,
                                       .focused = in == sclient,
                                   });
    }
  }

  wlr_renderer_end(renderer);

  wlr_output_commit(m->wlr_output);
}

static void configure_monitor(Monitor *m, struct wlr_output *o, int i, int x,
                              int y, int w, int h, int refresh) {
  m->position = i;
  m->wlr_output = o;
  m->tagset[0] = m->tagset[1] = 1;
  m->frame.notify = on_output_frame;
  m->destroy.notify = on_output_destroy;

  for_each(struct wlr_output_mode, o->modes) {
    if (it->width == w && it->height == h && it->refresh == refresh) {
      wlr_output_set_mode(o, it);
      break;
    }
  }
  wlr_output_enable_adaptive_sync(o, 1);

  wl_signal_add(&o->events.frame, &m->frame);
  wl_signal_add(&o->events.destroy, &m->destroy);

  Monitor *moni, *insertmon = NULL;
  wl_list_for_each(moni, &mons, link) {
    if (m->position > moni->position) {
      insertmon = moni;
    }
  }

  wl_list_insert(insertmon ? &insertmon->link : &mons, &m->link);

  wlr_output_enable(o, 1);
  if (wlr_output_commit(o)) {
    wlr_output_layout_add(output_layout, o, x, y);
    sgeom = *wlr_output_layout_get_box(output_layout, NULL);
    updatemons();
  }
}

void on_backend_new_output(struct wl_listener *listener, void *data) {
  log("%s", "on_backend_new_output");
  struct wlr_output *out = data;
  Monitor *m = out->data = calloc(1, sizeof(*m));

  if (!strcmp(out->name, "DP-3")) {
    configure_monitor(m, out, 0, 0, 0, 1920, 1080, 60000);
  } else if (!strcmp(out->name, "DP-2")) {
    configure_monitor(m, out, 1, 1920, 0, 1920, 1080, 60000);
  } else if (!strcmp(out->name, "DP-1")) {
    configure_monitor(m, out, 2, 3840, 0, 1920, 1080, 239760);
  }
}

// Ready to manage this surface
void on_xdg_surface_map(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_map");
  Client *c = wl_container_of(listener, c, map);
  if (c->type == X11Unmanaged) {
    wl_list_insert(&independents, &c->link);
  } else {
    wl_list_insert(&clients, &c->link);
  }

  setmon(c, smon, 0);
  focus(c);
}

// Stop managing this surface
void on_xdg_surface_unmap(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_unmap");
  Client *c = wl_container_of(listener, c, unmap);
  wl_list_remove(&c->link);
  if (c->type != X11Unmanaged) {
    setmon(c, NULL, 0);
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
  if (!c->mon) {
    c->mon = smon;
  }
  c->mon->fullscreen = c->mon->fullscreen ? NULL : c;
  wlr_xdg_toplevel_set_fullscreen(c->surface.xdg, c->mon->fullscreen);
  arrange(c->mon);
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

Monitor *dirtomon(int dir) {
  Monitor *m;
  return dir > 0 ? (smon->link.next == &mons
                        ? wl_container_of(mons.next, m, link)
                        : wl_container_of(smon->link.next, m, link))
                 : (smon->link.prev == &mons
                        ? wl_container_of(mons.prev, m, link)
                        : wl_container_of(smon->link.prev, m, link));
}

int focusmon(const int dir) {
  do {
    smon = dirtomon(dir);
  } while (!smon->wlr_output->enabled);
  return 1;
}

int focusstack(const int dir) {
  if (NULL == sclient) {
    return 1;
  }

  Client *c;
  if (dir > 0) {
    wl_list_for_each(c, &sclient->link, link) {
      if (&c->link == &clients) {
        continue;
      }
      if (visibleon(c, smon)) {
        break;
      }
    }
  } else {
    wl_list_for_each_reverse(c, &sclient->link, link) {
      if (&c->link == &clients) {
        continue;
      }
      if (visibleon(c, smon)) {
        break;
      }
    }
  }
  if (c) {
    focus(c);
  }
  return 1;
}

void sigchld(int unused) {
  if (signal(SIGCHLD, sigchld) == SIG_ERR) {
    fmt("%s", "can't install SIGCHLD handler");
  }
  while (0 < waitpid(-1, NULL, WNOHANG))
    ;
}

int spawn(const char *cmd) {
  if (fork() == 0) {
    setsid();
    execvp(cmd, (char *[]){NULL});
  }
  return 1;
}

int tag(const unsigned int tag) {
  if (sclient && tag & TAGMASK) {
    sclient->tags = tag & TAGMASK;
    arrange(smon);
  }
  return 1;
}

int tagmon(const unsigned int tag) {
  if (sclient) {
    setmon(sclient, dirtomon(tag), 0);
  }
  return 1;
}

int view(const unsigned int tag) {
  if ((tag & TAGMASK) == smon->tagset[smon->seltags]) {
    return 1;
  }
  smon->seltags ^= 1;
  if (tag & TAGMASK) {
    smon->tagset[smon->seltags] = tag & TAGMASK;
  }
  arrange(smon);
  return 1;
}

int zoom() {
  if (sclient) {
    wl_list_remove(&sclient->link);
    wl_list_insert(&clients, &sclient->link);
    arrange(smon);
  }
  return 1;
}

int kill_client() {
  if (sclient && sclient->type == XDGShell) {
    wlr_xdg_toplevel_send_close(sclient->surface.xdg);
  } else if (sclient) {
    wlr_xwayland_surface_close(sclient->surface.xwayland);
  }
  focus(xytoclient(cursor->x, cursor->y));
  return 1;
}

int handle_key(uint32_t code, uint32_t mods) {
  if (mods == WLR_MODIFIER_LOGO) {
    switch (code) {
      CASE(57, zoom());
      CASE(28, spawn("launcher"));
      CASE(25, spawn("passmenu"));
      CASE(46, focusstack(1));
      CASE(35, focusstack(-1));
      CASE(31, focusmon(1));
      CASE(20, focusmon(-1));
      CASE(23, view(1));
      CASE(18, view(2));
      CASE(24, view(4));
      CASE(49, view(8));
    }
  } else if (mods == (WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL)) {
    switch (code) {
      CASE(46, kill_client());
      CASE(28, spawn("alacritty"));
      CASE(31, tagmon(1));
      CASE(20, tagmon(-1));
      CASE(23, tag(1));
      CASE(18, tag(2));
      CASE(24, tag(4));
      CASE(49, tag(8));
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

void pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
                  uint32_t time) {
  if (c && !surface) {
    surface = client_surface(c);
  }

  if (!surface) {
    wlr_seat_pointer_notify_clear_focus(seat);
    return;
  }

  if (surface == seat->pointer_state.focused_surface) {
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    return;
  }

  wlr_seat_pointer_notify_enter(seat, surface, sx, sy);

  if (c && c->type != X11Unmanaged) {
    focus(c);
  }
}

void on_cursor_motion(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_motion *e = data;
  wlr_cursor_move(cursor, e->device, e->delta_x, e->delta_y);
  smon = xytomon(cursor->x, cursor->y);

  double sx = 0, sy = 0;
  struct wlr_surface *surface = NULL;
  Client *c = NULL;

  if (NULL != dclient) {
    set_geometry(dclient, cursor->x - grabcx, cursor->y - grabcy,
                 dclient->geom.width, dclient->geom.height);
    return;
  } else if ((c = xytoindependent(cursor->x, cursor->y))) {
    surface = wlr_surface_surface_at(
        c->surface.xwayland->surface, cursor->x - c->surface.xwayland->x,
        cursor->y - c->surface.xwayland->y, &sx, &sy);
  } else if ((c = xytoclient(cursor->x, cursor->y))) {
    surface = client_surface_at(c, cursor->x - c->geom.x, cursor->y - c->geom.y,
                                &sx, &sy);
  }

  pointerfocus(c, surface, sx, sy, e->time_msec);
}

void on_seat_request_set_cursor(struct wl_listener *listener, void *data) {
  log("%s", "on_seat_request_cursor");
  struct wlr_seat_pointer_request_set_cursor_event *e = data;
  wlr_cursor_set_surface(cursor, e->surface, e->hotspot_x, e->hotspot_y);
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

int main(int argc, char *argv[]) {
  wlr_log_init(WLR_INFO, NULL);
  assert(getenv("XDG_RUNTIME_DIR"));

  sigchld(0);

  wl_list_init(&mons);
  wl_list_init(&clients);
  wl_list_init(&independents);

  struct wl_display *display = wl_display_create();
  struct wlr_backend *backend = wlr_backend_autocreate(display);
  assert(backend);

  renderer = wlr_backend_get_renderer(backend);
  assert(wlr_renderer_init_wl_display(renderer, display));
  struct wlr_compositor *compositor = wlr_compositor_create(display, renderer);
  struct wlr_xcursor_manager *cursor_manager =
      wlr_xcursor_manager_create(NULL, 24);
  output_layout = wlr_output_layout_create();
  xdg_shell = wlr_xdg_shell_create(display);
  cursor = wlr_cursor_create();
  seat = wlr_seat_create(display, "seat0");
  assert(xwayland = wlr_xwayland_create(display, compositor, 1));

  wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER |
                                      WL_SEAT_CAPABILITY_KEYBOARD);
  wlr_cursor_attach_output_layout(cursor, output_layout);
  wlr_xcursor_manager_load(cursor_manager, 1);
  wlr_xcursor_manager_set_cursor_image(cursor_manager, "left_ptr", cursor);
  wlr_export_dmabuf_manager_v1_create(display);
  wlr_screencopy_manager_v1_create(display);
  wlr_data_control_manager_v1_create(display);
  wlr_data_device_manager_create(display);
  wlr_primary_selection_v1_device_manager_create(display);
  wlr_viewporter_create(display);
  wlr_xdg_output_manager_v1_create(display, output_layout);

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

  smon = xytomon(0, 0);

  wl_display_run(display);

  wlr_xwayland_destroy(xwayland);
  wl_display_destroy_clients(display);
  wlr_backend_destroy(backend);
  wlr_cursor_destroy(cursor);
  wlr_output_layout_destroy(output_layout);
  wlr_seat_destroy(seat);
  wl_display_destroy(display);
  return EXIT_SUCCESS;
}
