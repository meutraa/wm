#define _XOPEN_SOURCE 700
#include <X11/Xlib.h>
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
  struct wl_listener activate;  // xwayland only
  struct wl_listener configure; // xwayland only
  struct wlr_box geom;          // layout-relative
  Monitor *mon;
  unsigned int type;
  unsigned int tags;
  uint32_t resize; // configure serial of a pending resize
} Client;

typedef struct {
  struct wl_list link;
  struct wlr_input_device *device;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
} Keyboard;

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
};

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

#define CAT(x, y) CAT_(x, y)
#define CAT_(x, y) x##y
#define listen(S, F)                                                           \
  struct wl_listener CAT(F, listener) = {.notify = F};                         \
  wl_signal_add(S, &(CAT(F, listener)))

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
    struct wlr_box geom = {
        .x = it->surface.xwayland->x,
        .y = it->surface.xwayland->y,
        .width = it->surface.xwayland->width,
        .height = it->surface.xwayland->height,
    };
    if (wlr_box_contains_point(&geom, x, y)) {
      return it;
    }
  }
  return NULL;
}

static inline int client_is_x11(Client *c) {
  return c->type == X11Managed || c->type == X11Unmanaged;
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
  if (client_is_x11(c)) {
    wlr_surface_for_each_surface(c->surface.xwayland->surface, fn, data);
    return;
  }
  wlr_xdg_surface_for_each_surface(c->surface.xdg, fn, data);
}

static inline const char *client_get_appid(Client *c) {
  return client_is_x11(c) ? c->surface.xwayland->class
                          : c->surface.xdg->toplevel->app_id;
}

static inline void client_get_geometry(Client *c, struct wlr_box *geom) {
  if (client_is_x11(c)) {
    geom->x = c->surface.xwayland->x;
    geom->y = c->surface.xwayland->y;
    geom->width = c->surface.xwayland->width;
    geom->height = c->surface.xwayland->height;
    return;
  }
  wlr_xdg_surface_get_geometry(c->surface.xdg, geom);
}

static inline const char *client_get_title(Client *c) {
  return client_is_x11(c) ? c->surface.xwayland->title
                          : c->surface.xdg->toplevel->title;
}

static inline int client_is_unmanaged(Client *c) {
  return c->type == X11Unmanaged;
}

static inline uint32_t client_set_size(Client *c, uint32_t width,
                                       uint32_t height) {
  if (client_is_x11(c)) {
    wlr_xwayland_surface_configure(c->surface.xwayland, c->geom.x, c->geom.y,
                                   width, height);
    return 0;
  }
  return wlr_xdg_toplevel_set_size(c->surface.xdg, width, height);
}

static inline struct wlr_surface *client_surface(Client *c) {
  return client_is_x11(c) ? c->surface.xwayland->surface
                          : c->surface.xdg->surface;
}

static inline void llisten(struct wl_signal *sig, struct wl_listener *l,
                           wl_notify_func_t f) {
  l->notify = f;
  wl_list_insert(sig->listener_list.prev, &(l->link));
}

static inline struct wlr_surface *
client_surface_at(Client *c, double cx, double cy, double *sx, double *sy) {
  return client_is_x11(c)
             ? wlr_surface_surface_at(c->surface.xwayland->surface, cx, cy, sx,
                                      sy)
             : wlr_xdg_surface_surface_at(c->surface.xdg, cx, cy, sx, sy);
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

void arrange(Monitor *m) {
  unsigned int i, n = 0, h, mw, my, ty, nmaster = 1;
  Client *c;
  wl_list_for_each(c, &clients, link) {
    if (VISIBLEON(c, m) && (0 != strcmp(client_get_appid(c), "floating"))) {
      n++;
    }
  }

  log("%d windows", n);

  mw = n > nmaster ? nmaster ? m->w.width * 0.5 : 0 : m->w.width;
  i = my = ty = 0;
  for_each(Client, clients) {
    if (!VISIBLEON(it, m)) {
      log("%s", "not visible");
      continue;
    }

    if (0 == strcmp(client_get_appid(it), "floating")) {
      log("%s", "sizing floating window");
      set_geometry(it, m->w.x + 640, 360, 640, 360, 0);
      continue;
    }

    if (i < nmaster) {
      h = (m->w.height - my) / (MIN(n, nmaster) - i);
      set_geometry(it, m->w.x, m->w.y + my, mw, h, 0);
      my += it->geom.height;
    } else {
      h = (m->w.height - ty) / (n - i);
      set_geometry(it, m->w.x + mw, m->w.y + ty, m->w.width - mw, h, 0);
      ty += it->geom.height;
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

  if (oldmon) {
    wlr_surface_send_leave(client_surface(c), oldmon->wlr_output);
    arrange(oldmon);
  }
  if (m) {
    applybounds(c, &m->m);
    wlr_surface_send_enter(client_surface(c), m->wlr_output);
    c->tags = newtags ? newtags : m->tagset[m->seltags];
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

void on_input_destroy(struct wl_listener *listener, void *data) {
  log("%s", "on_input_destroy");
  struct wlr_input_device *device = data;
  Keyboard *kb = device->data;
  wl_list_remove(&kb->modifiers.link);
  wl_list_remove(&kb->key.link);
  wl_list_remove(&kb->destroy.link);
  free(kb);
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

  for_each(struct wlr_output_mode, o->modes) {
    if (it->width == w && it->height == h && it->refresh == refresh) {
      wlr_output_set_mode(o, it);
      break;
    }
  }
  wlr_output_enable_adaptive_sync(o, 1);

  llisten(&o->events.frame, &m->frame, on_output_frame);
  llisten(&o->events.destroy, &m->destroy, on_output_destroy);

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

  if (!strcmp(out->name, "DP-1")) {
    configure_monitor(m, out, 2, 3840, 0, 1920, 1080, 60000);
  } else if (!strcmp(out->name, "DP-2")) {
    configure_monitor(m, out, 1, 1920, 0, 1920, 1080, 60000);
  } else if (!strcmp(out->name, "DP-3")) {
    configure_monitor(m, out, 0, 0, 0, 1920, 1080, 239760);
  }
}

void on_xdg_surface_map(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_map");
  Client *c = wl_container_of(listener, c, map);

  if (client_is_unmanaged(c)) {
    wl_list_insert(&independents, &c->link);
    return;
  }

  wl_list_insert(&clients, &c->link);
  wl_list_insert(&fstack, &c->flink);
  wl_list_insert(&stack, &c->slink);

  client_get_geometry(c, &c->geom);
  setmon(c, selmon, 0);
}

void on_xdg_surface_unmap(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_surface_unmap");
  Client *c = wl_container_of(listener, c, unmap);
  wl_list_remove(&c->link);
  if (!client_is_unmanaged(c)) {
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
  }
  free(c);
}

void on_xdg_new_surface(struct wl_listener *listener, void *data) {
  log("%s", "on_xdg_new_surface");
  struct wlr_xdg_surface *xdg_surface = data;
  Client *c;

  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    c = xdg_surface->data = calloc(1, sizeof(*c));
    c->surface.xdg = xdg_surface;

    // Tells the surface it is tiled
    wlr_xdg_toplevel_set_tiled(c->surface.xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
                                                   WLR_EDGE_LEFT |
                                                   WLR_EDGE_RIGHT);

    // Do I even need commit?
    llisten(&xdg_surface->surface->events.commit, &c->commit,
            on_xdg_surface_commit);
    llisten(&xdg_surface->events.map, &c->map, on_xdg_surface_map);
    llisten(&xdg_surface->events.unmap, &c->unmap, on_xdg_surface_unmap);
    llisten(&xdg_surface->events.destroy, &c->destroy, on_xdg_surface_destroy);
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
  log("%s", "tagmon");
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

int handle_key_press(uint32_t mods, uint32_t code) {
  // log("KEY: %i", code);
  Client *sel;
  if (mods == WLR_MODIFIER_LOGO) {
    switch (code) {
    case 28:
      return spawn("launcher");
    case 25:
      return spawn("passmenu");
    case 57:
      sel = selclient();
      if (sel) {
        wl_list_remove(&sel->link);
        wl_list_insert(&clients, &sel->link);
        focusclient(sel, 1);
        arrange(selmon);
      }
      return 1;
    case 46:
      return focusstack(1);
    case 35:
      return focusstack(-1);
    case 31:
      return focusmon(1);
    case 20:
      return focusmon(-1);
    case 23:
      return view(1);
    case 18:
      return view(2);
    case 24:
      return view(4);
    case 49:
      return view(8);
    }
  } else if (mods == (WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL)) {
    switch (code) {
    case 28:
      return spawn("alacritty");
    case 46:
      sel = selclient();
      if (sel) {
        if (client_is_x11(sel)) {
          wlr_xwayland_surface_close(sel->surface.xwayland);
          return 1;
        }
        wlr_xdg_toplevel_send_close(sel->surface.xdg);
      }
      return 1;
    case 31:
      return tagmon(1);
    case 20:
      return tagmon(-1);
    case 23:
      return tag(1);
    case 18:
      return tag(2);
    case 24:
      return tag(4);
    case 49:
      return tag(8);
    }
  }
  return 0;
}

void on_keyboard_key(struct wl_listener *listener, void *data) {
  log("%s", "on_keyboard_key");
  Keyboard *kb = wl_container_of(listener, kb, key);
  struct wlr_event_keyboard_key *event = data;

  uint32_t mods = wlr_keyboard_get_modifiers(kb->device->keyboard);

  if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
      handle_key_press(mods, event->keycode)) {
    return;
  }

  wlr_seat_set_keyboard(seat, kb->device);
  wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
                               event->state);
}

void on_keyboard_modifiers(struct wl_listener *listener, void *data) {
  log("%s", "on_keyboard_modifiers");
  Keyboard *kb = wl_container_of(listener, kb, modifiers);
  wlr_seat_set_keyboard(seat, kb->device);
  wlr_seat_keyboard_notify_modifiers(seat, &kb->device->keyboard->modifiers);
}

void on_backend_new_input(struct wl_listener *listener, void *data) {
  log("%s", "on_backend_new_input");
  struct wlr_input_device *device = data;

  if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
    struct xkb_context *context;
    struct xkb_keymap *keymap;
    Keyboard *kb = device->data = calloc(1, sizeof(*kb));
    kb->device = device;

    context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    keymap = xkb_map_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(device->keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(device->keyboard, 25, 220);

    llisten(&device->keyboard->events.modifiers, &kb->modifiers,
            on_keyboard_modifiers);
    llisten(&device->keyboard->events.key, &kb->key, on_keyboard_key);
    llisten(&device->events.destroy, &kb->destroy, on_input_destroy);

    wlr_seat_set_keyboard(seat, device);
  } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
    wlr_cursor_attach_input_device(cursor, device);
  }

  wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER |
                                      WL_SEAT_CAPABILITY_KEYBOARD);
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

  if (!c || client_is_unmanaged(c)) {
    return;
  }

  focusclient(c, 0);
}

void motionnotify(uint32_t time) {}

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
  wl_signal_add(&xwayland_surface->events.map, &c->map);
  llisten(&xwayland_surface->events.unmap, &c->unmap, on_xdg_surface_unmap);
  llisten(&xwayland_surface->events.request_activate, &c->activate,
          on_xwayland_surface_request_activate);
  llisten(&xwayland_surface->events.request_configure, &c->configure,
          on_xwayland_surface_request_configure);
  llisten(&xwayland_surface->events.destroy, &c->destroy,
          on_xdg_surface_destroy);
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

  if (!getenv("XDG_RUNTIME_DIR")) {
    panic("%s", "XDG_RUNTIME_DIR must be set");
  }

  struct wl_display *display = wl_display_create();

  sigchld(0);

  struct wlr_backend *backend = wlr_backend_autocreate(display);
  if (!backend) {
    panic("%s", "couldn't create backend");
  }

  renderer = wlr_backend_get_renderer(backend);
  wlr_renderer_init_wl_display(renderer, display);

  struct wlr_compositor *compositor = wlr_compositor_create(display, renderer);
  wlr_export_dmabuf_manager_v1_create(display);
  wlr_screencopy_manager_v1_create(display);
  wlr_data_control_manager_v1_create(display);
  wlr_data_device_manager_create(display);
  wlr_primary_selection_v1_device_manager_create(display);
  wlr_viewporter_create(display);

  output_layout = wlr_output_layout_create();
  wlr_xdg_output_manager_v1_create(display, output_layout);

  wl_list_init(&mons);
  listen(&backend->events.new_output, on_backend_new_output);

  wl_list_init(&clients);
  wl_list_init(&fstack);
  wl_list_init(&stack);
  wl_list_init(&independents);

  xdg_shell = wlr_xdg_shell_create(display);
  listen(&xdg_shell->events.new_surface, on_xdg_new_surface);

  cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(cursor, output_layout);

  listen(&cursor->events.motion, on_cursor_motion);
  listen(&cursor->events.button, on_cursor_button);
  listen(&cursor->events.axis, on_cursor_axis);
  listen(&cursor->events.frame, on_cursor_frame);

  listen(&backend->events.new_input, on_backend_new_input);
  seat = wlr_seat_create(display, "seat0");
  listen(&seat->events.request_set_cursor, on_seat_request_set_cursor);
  listen(&seat->events.request_set_selection, on_seat_request_set_selection);
  listen(&seat->events.request_set_primary_selection,
         on_seat_request_set_primary_selection);

  xwayland = wlr_xwayland_create(display, compositor, 1);
  if (!xwayland) {
    err("%s", "failed to create XWayland server");
  } else {
    listen(&xwayland->events.ready, on_xwayland_ready);
    listen(&xwayland->events.new_surface, on_xwayland_new_surface);
    setenv("DISPLAY", xwayland->display_name, 1);
  }

  const char *socket = wl_display_add_socket_auto(display);
  if (!socket) {
    panic("%s", "startup: display_add_socket_auto");
  }
  setenv("WAYLAND_DISPLAY", socket, 1);

  if (!wlr_backend_start(backend)) {
    panic("%s", "unable to start backend");
  }

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
