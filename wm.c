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
  struct wl_list flink;
  struct wl_list slink;
  union {
    struct wlr_xdg_surface *xdg;
    struct wlr_xwayland_surface *xwayland;
  } surface;
  struct wl_listener commit;
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
  uint32_t resize; // configure serial of a pending resize
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

static struct wlr_renderer *renderer;

static struct wlr_xdg_shell *xdg_shell;

static struct wl_list clients; // tiling order
static struct wl_list fstack;  // focus order
static struct wl_list stack;   // stacking z-order
static struct wl_list independents;
static struct wl_list mons;

static struct wlr_xwayland *xwayland;

static struct wlr_cursor *cursor;

static struct wlr_seat *seat;
static unsigned int dragging;
static Client *dragged_client;
static int grabcx, grabcy;

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static Monitor *selmon;

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define VISIBLEON(C, M)                                                        \
  ((C)->mon == (M) && ((C)->tags & (M)->tagset[(M)->seltags]))
#define LENGTH(X) (sizeof X / sizeof X[0])
#define END(A) ((A) + LENGTH(A))
#define TAGMASK ((1 << LENGTH(tags)) - 1)
#define log(fmt, ...) wlr_log(WLR_INFO, fmt, ##__VA_ARGS__)
#define err(fmt, ...) wlr_log(WLR_ERROR, fmt, ##__VA_ARGS__)
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

Client *xytoclient(double x, double y) {
  Client *c;
  wl_list_for_each(c, &stack, slink) {
    if (VISIBLEON(c, c->mon) && wlr_box_contains_point(&c->geom, x, y)) {
      return c;
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

static inline void
client_for_each_surface(Client *c, wlr_surface_iterator_func_t fn, void *data) {
  if (c->type == XDGShell) {
    wlr_xdg_surface_for_each_surface(c->surface.xdg, fn, data);
    return;
  }
  wlr_surface_for_each_surface(c->surface.xwayland->surface, fn, data);
}

static inline const char *client_get_appid(Client *c) {
  return c->type == XDGShell ? c->surface.xdg->toplevel->app_id
                             : c->surface.xwayland->class;
}

static inline void client_get_geometry(Client *c, struct wlr_box *geom) {
  if (c->type == XDGShell) {
    wlr_xdg_surface_get_geometry(c->surface.xdg, geom);
    return;
  }
  geom->x = c->surface.xwayland->x;
  geom->y = c->surface.xwayland->y;
  geom->width = c->surface.xwayland->width;
  geom->height = c->surface.xwayland->height;
}

static inline void client_close(Client *c) {
  if (c->type == XDGShell) {
    wlr_xdg_toplevel_send_close(c->surface.xdg);
    return;
  }
  wlr_xwayland_surface_close(c->surface.xwayland);
}

static inline uint32_t client_set_size(Client *c, uint32_t width,
                                       uint32_t height) {
  if (c->type == XDGShell) {
    return wlr_xdg_toplevel_set_size(c->surface.xdg, width, height);
  }
  wlr_xwayland_surface_configure(c->surface.xwayland, c->geom.x, c->geom.y,
                                 width, height);
  return 0;
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

Client *focustop(Monitor *m) {
  Client *c;
  wl_list_for_each(c, &fstack, flink) if (VISIBLEON(c, m)) return c;
  return NULL;
}

Client *selclient(void) {
  Client *c = wl_container_of(fstack.next, c, flink);
  if (wl_list_empty(&fstack) || !VISIBLEON(c, selmon)) {
    return NULL;
  }
  return c;
}

void applybounds(Client *c, struct wlr_box *bbox) {
  c->geom.width = MAX(1, c->geom.width);
  c->geom.height = MAX(1, c->geom.height);

  if (c->geom.x >= bbox->x + bbox->width) {
    c->geom.x = bbox->x + bbox->width - c->geom.width;
  }
  if (c->geom.y >= bbox->y + bbox->height) {
    c->geom.y = bbox->y + bbox->height - c->geom.height;
  }
  if (c->geom.x + c->geom.width <= bbox->x) {
    c->geom.x = bbox->x;
  }
  if (c->geom.y + c->geom.height <= bbox->y) {
    c->geom.y = bbox->y;
  }
}

void set_geometry(Client *c, int x, int y, int w, int h, int interact) {
  struct wlr_box *bbox = interact ? &sgeom : &c->mon->w;
  c->geom.x = x;
  c->geom.y = y;
  c->geom.width = w;
  c->geom.height = h;
  applybounds(c, bbox);
  c->resize = client_set_size(c, c->geom.width, c->geom.height);
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
    if (VISIBLEON(c, m) && !isfloating(c)) {
      n++;
    }
  }

  int i = 0;
  for_each(Client, clients) {
    if (!VISIBLEON(it, m)) {
      continue;
    }

    if (it->mon->fullscreen == it) {
      log("%s: %d+%d-%dx%d", "arranging full screen client", m->m.x, m->m.y,
          m->m.width, m->m.height);
      set_geometry(it, m->m.x, m->m.y, m->m.width, m->m.height, 0);
      continue;
    }

    if (isfloating(it)) {
      set_geometry(it, m->w.x + 640, 360, 640, 360, 0);
      continue;
    }

    int sidewidth = m->m.width / n;
    sidewidth = sidewidth == m->m.width ? 0 : sidewidth;
    int mainwidth = m->m.width - sidewidth;
    if (i == 0) {
      set_geometry(it, m->m.x, m->m.y, mainwidth, m->m.height, 0);
    } else {
      int sideheight = m->m.height / (n - 1);
      int sidey = sideheight * (i - 1);
      set_geometry(it, m->m.x + mainwidth, sidey, sidewidth, sideheight, 0);
    }
    i++;
  }
}

void focusclient(Client *c, int lift) {
  if (c && lift) {
    wl_list_remove(&c->slink);
    wl_list_insert(&stack, &c->slink);
  }

  struct wlr_surface *old = seat->keyboard_state.focused_surface;
  if (c && client_surface(c) == old) {
    return;
  }

  if (c) {
    wl_list_remove(&c->flink);
    wl_list_insert(&fstack, &c->flink);
    selmon = c->mon;
  }

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
    applybounds(c, &m->m);
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
  focusclient(focustop(selmon), 1);
}

void updatemons() {
  sgeom = *wlr_output_layout_get_box(output_layout, NULL);
  for_each(Monitor, mons) {
    it->m = it->w = *wlr_output_layout_get_box(output_layout, it->wlr_output);
    arrange(it);
  }
}

void on_cursor_axis(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_axis *event = data;
  wlr_seat_pointer_notify_axis(seat, event->time_msec, event->orientation,
                               event->delta, event->delta_discrete,
                               event->source);
}

void on_cursor_button(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_button *event = data;

  if (event->state == WLR_BUTTON_PRESSED && event->button == BTN_SIDE) {
    dragged_client = xytoclient(cursor->x, cursor->y);
    if (dragged_client) {
      dragging = 1;
      focusclient(dragged_client, 1);
      grabcx = cursor->x - dragged_client->geom.x;
      grabcy = cursor->y - dragged_client->geom.y;
    }
    return;
  } else if (WLR_BUTTON_RELEASED == event->state && dragging) {
    dragging = 0;
    selmon = xytomon(cursor->x, cursor->y);
    setmon(dragged_client, selmon, 0);
    return;
  }

  wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
                                 event->state);
}

void closemon(Monitor *m) {
  for_each(Client, clients) {
    if (it->geom.x > m->m.width) {
      set_geometry(it, it->geom.x - m->w.width, it->geom.y, it->geom.width,
                   it->geom.height, 0);
    }
    if (it->mon == m) {
      setmon(it, selmon, it->tags);
    }
  }
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
    selmon = wl_container_of(mons.prev, selmon, link);
  while (!selmon->wlr_output->enabled && i++ < nmons);
  focusclient(focustop(selmon), 1);
  closemon(m);
  free(m);
}

void on_xdg_surface_commit(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, commit);
  if (c->resize && c->resize <= c->surface.xdg->configure_serial) {
    c->resize = 0;
  }
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

void renderclients(Monitor *m, struct timespec *now) {
  Client *c, *sel = selclient();
  double ox, oy;
  struct render_data rdata;
  wl_list_for_each_reverse(c, &stack, slink) {
    if (!VISIBLEON(c, c->mon) ||
        !wlr_output_layout_intersects(output_layout, m->wlr_output, &c->geom)) {
      continue;
    }

    ox = c->geom.x, oy = c->geom.y;
    wlr_output_layout_output_coords(output_layout, m->wlr_output, &ox, &oy);

    rdata.output = m->wlr_output;
    rdata.when = now;
    rdata.x = c->geom.x;
    rdata.y = c->geom.y;
    rdata.focused = c == sel;
    client_for_each_surface(c, render, &rdata);
  }
}

void renderindependents(struct wlr_output *output, struct timespec *now) {
  Client *sel = selclient();
  struct render_data rdata;
  struct wlr_box geom;

  for_each_reverse(Client, independents) {
    geom.x = it->surface.xwayland->x;
    geom.y = it->surface.xwayland->y;
    geom.width = it->surface.xwayland->width;
    geom.height = it->surface.xwayland->height;

    if (wlr_output_layout_intersects(output_layout, output, &geom)) {
      rdata.output = output;
      rdata.when = now;
      rdata.x = it->surface.xwayland->x;
      rdata.y = it->surface.xwayland->y;
      rdata.focused = it == sel;
      wlr_surface_for_each_surface(it->surface.xwayland->surface, render,
                                   &rdata);
    }
  }
}

void on_output_frame(struct wl_listener *listener, void *data) {
  Monitor *m = wl_container_of(listener, m, frame);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  if (!wlr_output_attach_render(m->wlr_output, NULL)) {
    return;
  }

  wlr_renderer_begin(renderer, m->wlr_output->width, m->wlr_output->height);
  wlr_renderer_clear(renderer, (float[]){0.0, 0.0, 0.0, 1.0});

  renderclients(m, &now);
  renderindependents(m->wlr_output, &now);
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

void on_xdg_surface_map(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_map");
  Client *c = wl_container_of(listener, c, map);

  if (c->type == X11Unmanaged) {
    wl_list_insert(&independents, &c->link);
    return;
  }

  wl_list_insert(&clients, &c->link);
  wl_list_insert(&fstack, &c->flink);
  wl_list_insert(&stack, &c->slink);

  client_get_geometry(c, &c->geom);
  log("%s", "before crash");
  setmon(c, selmon, 0);
  log("%s", "after crash");
}

void on_xdg_surface_unmap(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_unmap");
  Client *c = wl_container_of(listener, c, unmap);
  wl_list_remove(&c->link);
  if (c->type != X11Unmanaged) {
    setmon(c, NULL, 0);
    wl_list_remove(&c->flink);
    wl_list_remove(&c->slink);
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
    wl_list_remove(&c->commit.link);
    wl_list_remove(&c->fullscreen.link);
  }
  free(c);
}

void on_xdg_surface_fullscreen(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_fullscreen");
  Client *c = wl_container_of(listener, c, fullscreen);
  if (NULL == c->mon) {
    c->mon = selmon;
  }
  c->mon->fullscreen = c->mon->fullscreen ? NULL : c;
  wlr_xdg_toplevel_set_fullscreen(c->surface.xdg, c->mon->fullscreen);
  arrange(c->mon);
}

void on_xdg_new_surface(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_new_surface");
  struct wlr_xdg_surface *xdg_surface = data;

  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    Client *c = xdg_surface->data = calloc(1, sizeof(*c));
    c->surface.xdg = xdg_surface;
    c->type = XDGShell;
    c->commit.notify = on_xdg_surface_commit;
    c->map.notify = on_xdg_surface_map;
    c->unmap.notify = on_xdg_surface_unmap;
    c->destroy.notify = on_xdg_surface_destroy;
    c->fullscreen.notify = on_xdg_surface_fullscreen;

    // Tells the surface it is tiled
    wlr_xdg_toplevel_set_tiled(c->surface.xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
                                                   WLR_EDGE_LEFT |
                                                   WLR_EDGE_RIGHT);

    wl_signal_add(&xdg_surface->surface->events.commit, &c->commit);
    wl_signal_add(&xdg_surface->events.map, &c->map);
    wl_signal_add(&xdg_surface->events.unmap, &c->unmap);
    wl_signal_add(&xdg_surface->events.destroy, &c->destroy);
    wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen,
                  &c->fullscreen);
  }
}

void on_cursor_frame(struct wl_listener *listener, void *data) {
  wlr_seat_pointer_notify_frame(seat);
}

Monitor *dirtomon(int dir) {
  Monitor *m;
  return dir > 0 ? (selmon->link.next == &mons
                        ? wl_container_of(mons.next, m, link)
                        : wl_container_of(selmon->link.next, m, link))
                 : (selmon->link.prev == &mons
                        ? wl_container_of(mons.prev, m, link)
                        : wl_container_of(selmon->link.prev, m, link));
}

int focusmon(const int dir) {
  do {
    selmon = dirtomon(dir);
  } while (!selmon->wlr_output->enabled);
  focusclient(focustop(selmon), 1);
  return 1;
}

int focusstack(const int dir) {
  Client *c, *sel = selclient();
  if (!sel) {
    return 1;
  }
  if (dir > 0) {
    wl_list_for_each(c, &sel->link, link) {
      if (&c->link == &clients) {
        continue;
      }
      if (VISIBLEON(c, selmon)) {
        break;
      }
    }
  } else {
    wl_list_for_each_reverse(c, &sel->link, link) {
      if (&c->link == &clients) {
        continue;
      }
      if (VISIBLEON(c, selmon)) {
        break;
      }
    }
  }
  focusclient(c, 1);
  return 1;
}

void sigchld(int unused) {
  if (signal(SIGCHLD, sigchld) == SIG_ERR) {
    err("%s", "can't install SIGCHLD handler");
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
  Client *sel = selclient();
  if (sel && tag & TAGMASK) {
    sel->tags = tag & TAGMASK;
    focusclient(focustop(selmon), 1);
    arrange(selmon);
  }
  return 1;
}

int tagmon(const unsigned int tag) {
  Client *sel = selclient();
  if (sel) {
    setmon(sel, dirtomon(tag), 0);
  }
  return 1;
}

int view(const unsigned int tag) {
  if ((tag & TAGMASK) == selmon->tagset[selmon->seltags]) {
    return 1;
  }
  selmon->seltags ^= 1;
  if (tag & TAGMASK) {
    selmon->tagset[selmon->seltags] = tag & TAGMASK;
  }
  focusclient(focustop(selmon), 1);
  arrange(selmon);
  return 1;
}

int zoom() {
  Client *sel = selclient();
  if (sel) {
    wl_list_remove(&sel->link);
    wl_list_insert(&clients, &sel->link);
    focusclient(sel, 1);
    arrange(selmon);
  }
  return 1;
}

int kill_client() {
  Client *sel = selclient();
  if (sel) {
    client_close(sel);
  }
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
  struct wlr_event_keyboard_key *event = data;
  uint32_t mods = wlr_keyboard_get_modifiers(input->device->keyboard);
  if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
      handle_key(event->keycode, mods)) {
    return;
  }

  wlr_seat_set_keyboard(seat, input->device);
  wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
                               event->state);
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
  // WHY
  // wl_list_remove(&input->link);
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
    focusclient(c, 0);
  }
}

void on_cursor_motion(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_motion *event = data;
  wlr_cursor_move(cursor, event->device, event->delta_x, event->delta_y);
  selmon = xytomon(cursor->x, cursor->y);

  double sx = 0, sy = 0;
  struct wlr_surface *surface = NULL;
  Client *c = NULL;

  if (dragging) {
    set_geometry(dragged_client, cursor->x - grabcx, cursor->y - grabcy,
                 dragged_client->geom.width, dragged_client->geom.height, 1);
    return;
  } else if ((c = xytoindependent(cursor->x, cursor->y))) {
    surface = wlr_surface_surface_at(
        c->surface.xwayland->surface, cursor->x - c->surface.xwayland->x,
        cursor->y - c->surface.xwayland->y, &sx, &sy);
  } else if ((c = xytoclient(cursor->x, cursor->y))) {
    surface = client_surface_at(c, cursor->x - c->geom.x, cursor->y - c->geom.y,
                                &sx, &sy);
  }

  pointerfocus(c, surface, sx, sy, event->time_msec);
}

void on_seat_request_set_cursor(struct wl_listener *listener, void *data) {
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  if (!dragging && event->seat_client == seat->pointer_state.focused_client) {
    wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x,
                           event->hotspot_y);
  }
}

void on_seat_request_set_primary_selection(struct wl_listener *listener,
                                           void *data) {
  log("%s", "on_seat_set_primary_selection");
  struct wlr_seat_request_set_primary_selection_event *event = data;
  wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void on_seat_request_set_selection(struct wl_listener *listener, void *data) {
  log("%s", "on_seat_request_set_selection");
  struct wlr_seat_request_set_selection_event *event = data;
  wlr_seat_set_selection(seat, event->source, event->serial);
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
  struct wlr_xwayland_surface_configure_event *event = data;
  wlr_xwayland_surface_configure(c->surface.xwayland, event->x, event->y,
                                 event->width, event->height);
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
  wl_list_init(&fstack);
  wl_list_init(&stack);
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

  wlr_seat_pointer_warp(seat, 0, 0);
  selmon = xytomon(cursor->x, cursor->y);

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
