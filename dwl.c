#define _XOPEN_SOURCE 700
#include <getopt.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
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
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
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
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <X11/Xlib.h>
#include <wlr/xwayland.h>
#endif

/* macros */
#define BARF(fmt, ...)                                                         \
  do {                                                                         \
    fprintf(stderr, fmt "\n", ##__VA_ARGS__);                                  \
    exit(EXIT_FAILURE);                                                        \
  } while (0)
#define EBARF(fmt, ...) BARF(fmt ": %s", ##__VA_ARGS__, strerror(errno))
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define VISIBLEON(C, M)                                                        \
  ((C)->mon == (M) && ((C)->tags & (M)->tagset[(M)->seltags]))
#define LENGTH(X) (sizeof X / sizeof X[0])
#define END(A) ((A) + LENGTH(A))
#define TAGMASK ((1 << LENGTH(tags)) - 1)
#define LISTEN(E, L, H) wl_signal_add((E), ((L)->notify = (H), (L)))
#define onselected()                                                           \
  Client *sel = selclient();                                                   \
  if (sel)

/* enums */
enum { CurNormal, CurMove }; /* cursor */

#ifdef XWAYLAND
enum { XDGShell, X11Managed, X11Unmanaged }; /* client types */
#endif

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
  struct wlr_box geom; /* layout-relative, includes border */
  Monitor *mon;
#ifdef XWAYLAND
  unsigned int type;
  struct wl_listener activate;
  struct wl_listener configure;
#endif
  unsigned int tags;
  uint32_t resize; /* configure serial of a pending resize */
} Client;

typedef struct {
  struct wl_list link;
  struct wlr_input_device *device;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
} Keyboard;

typedef struct {
  struct wlr_layer_surface_v1 *layer_surface;
  struct wl_list link;

  struct wl_listener destroy;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener surface_commit;

  struct wlr_box geo;
  enum zwlr_layer_shell_v1_layer layer;
} LayerSurface;

struct Monitor {
  struct wl_list link;
  struct wlr_output *wlr_output;
  struct wl_listener frame;
  struct wl_listener destroy;
  struct wlr_box m;         /* monitor area, layout-relative */
  struct wlr_box w;         /* window area, layout-relative */
  struct wl_list layers[4]; // LayerSurface::link
  unsigned int seltags;
  unsigned int tagset[2];
  int position;
};

typedef struct {
  const char *name;
  int x;
  int y;
  int w;
  int h;
  int refresh;
} MonitorRule;

/* Used to move all of the data necessary to render a surface from the top-level
 * frame handler to the per-surface render function. */
struct render_data {
  struct wlr_output *output;
  struct timespec *when;
  int x, y; /* layout-relative */
};

/* function declarations */
static void applybounds(Client *c, struct wlr_box *bbox);
static void arrange(Monitor *m);
static void arrangelayer(Monitor *m, struct wl_list *list,
                         struct wlr_box *usable_area, int exclusive);
static void arrangelayers(Monitor *m);
static void axisnotify(struct wl_listener *listener, void *data);
static void buttonpress(struct wl_listener *listener, void *data);
static void cleanup(void);
static void cleanupkeyboard(struct wl_listener *listener, void *data);
static void cleanupmon(struct wl_listener *listener, void *data);
static void closemon(Monitor *m);
static void commitlayersurfacenotify(struct wl_listener *listener, void *data);
static void commitnotify(struct wl_listener *listener, void *data);
static void createkeyboard(struct wlr_input_device *device);
static void createmon(struct wl_listener *listener, void *data);
static void createnotify(struct wl_listener *listener, void *data);
static void createlayersurface(struct wl_listener *listener, void *data);
static void createpointer(struct wlr_input_device *device);
static void cursorframe(struct wl_listener *listener, void *data);
static void destroylayersurfacenotify(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static Monitor *dirtomon(int dir);
static void focusclient(Client *c, int lift);
static void focusmon(const int dir);
static void focusstack(const int dir);
static Client *focustop(Monitor *m);
static void inputdevice(struct wl_listener *listener, void *data);
static int keybinding(uint32_t mods, xkb_keysym_t sym);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static void killclient();
static void maplayersurfacenotify(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static void motionnotify(uint32_t time);
static void motionrelative(struct wl_listener *listener, void *data);
static void move();
static void pointerfocus(Client *c, struct wlr_surface *surface, double sx,
                         double sy, uint32_t time);
static void render(struct wlr_surface *surface, int sx, int sy, void *data);
static void renderclients(Monitor *m, struct timespec *now);
static void renderlayer(struct wl_list *layer_surfaces, struct timespec *now);
static void rendermon(struct wl_listener *listener, void *data);
static void setsize(Client *c, int x, int y, int w, int h, int interact);
static void run();
static Client *selclient(void);
static void setcursor(struct wl_listener *listener, void *data);
static void setpsel(struct wl_listener *listener, void *data);
static void setsel(struct wl_listener *listener, void *data);
static void setmode(struct wlr_output *output, int width, int height,
                    int refresh);
static void setmon(Client *c, Monitor *m, unsigned int newtags);
static void setup(void);
static void sigchld(int unused);
static void spawn(const char *cmd);
static void tag(const unsigned int tag);
static void tagmon(const unsigned int tag);
static void unmaplayersurface(LayerSurface *layersurface);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updatemons();
static void view(const unsigned int tag);
static Client *xytoclient(double x, double y);
static struct wlr_surface *xytolayersurface(struct wl_list *layer_surfaces,
                                            double x, double y, double *sx,
                                            double *sy);
static Monitor *xytomon(double x, double y);
static void zoom();

static struct wl_display *dpy;
static struct wlr_backend *backend;
static struct wlr_renderer *drw;
static struct wlr_compositor *compositor;

static struct wlr_xdg_shell *xdg_shell;
static struct wl_list clients; /* tiling order */
static struct wl_list fstack;  /* focus order */
static struct wl_list stack;   /* stacking z-order */
static struct wl_list independents;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;

static struct wlr_cursor *cursor;

static struct wlr_seat *seat;
static struct wl_list keyboards;
static unsigned int cursor_mode;
static Client *grabc;
static int grabcx, grabcy; /* client-relative */

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static struct wl_list mons;
static Monitor *selmon;

/* global event handlers */
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener new_input = {.notify = inputdevice};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_xdg_surface = {.notify = createnotify};
static struct wl_listener new_layer_shell_surface = {.notify =
                                                         createlayersurface};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static void renderindependents(struct wlr_output *output, struct timespec *now);
static void xwaylandready(struct wl_listener *listener, void *data);
static Client *xytoindependent(double x, double y);
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};
static struct wlr_xwayland *xwayland;
#endif

static void outputmgrapply(struct wl_listener *listener, void *data);
static struct wlr_output_manager_v1 *output_mgr;
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};

static const char *tags[] = {"i", "e", "o", "n"};

static const MonitorRule monrules[] = {
    {"DP-3", 0, 0, 1920, 1080, 239760},
    {"DP-2", 1920, 0, 0, 0, 0},
    {"DP-1", 3840, 0, 1920, 1080, 60000},
};

#define CASE(A, B)                                                             \
  case A:                                                                      \
    B;                                                                         \
    return 1;

/* Leave this function first; it's used in the others */
static inline int client_is_x11(Client *c) {
#ifdef XWAYLAND
  return c->type == X11Managed || c->type == X11Unmanaged;
#else
  return 0;
#endif
}

/* The others */
static inline void client_activate_surface(struct wlr_surface *s,
                                           int activated) {
#ifdef XWAYLAND
  if (wlr_surface_is_xwayland_surface(s)) {
    wlr_xwayland_surface_activate(wlr_xwayland_surface_from_wlr_surface(s),
                                  activated);
    return;
  }
#endif
  if (wlr_surface_is_xdg_surface(s))
    wlr_xdg_toplevel_set_activated(wlr_xdg_surface_from_wlr_surface(s),
                                   activated);
}

static inline void
client_for_each_surface(Client *c, wlr_surface_iterator_func_t fn, void *data) {
#ifdef XWAYLAND
  if (client_is_x11(c)) {
    wlr_surface_for_each_surface(c->surface.xwayland->surface, fn, data);
    return;
  }
#endif
  wlr_xdg_surface_for_each_surface(c->surface.xdg, fn, data);
}

static inline const char *client_get_appid(Client *c) {
#ifdef XWAYLAND
  if (client_is_x11(c))
    return c->surface.xwayland->class;
#endif
  return c->surface.xdg->toplevel->app_id;
}

static inline void client_get_geometry(Client *c, struct wlr_box *geom) {
#ifdef XWAYLAND
  if (client_is_x11(c)) {
    geom->x = c->surface.xwayland->x;
    geom->y = c->surface.xwayland->y;
    geom->width = c->surface.xwayland->width;
    geom->height = c->surface.xwayland->height;
    return;
  }
#endif
  wlr_xdg_surface_get_geometry(c->surface.xdg, geom);
}

static inline const char *client_get_title(Client *c) {
#ifdef XWAYLAND
  if (client_is_x11(c))
    return c->surface.xwayland->title;
#endif
  return c->surface.xdg->toplevel->title;
}

static inline int client_is_unmanaged(Client *c) {
#ifdef XWAYLAND
  return c->type == X11Unmanaged;
#endif
  return 0;
}

static inline void client_send_close(Client *c) {
#ifdef XWAYLAND
  if (client_is_x11(c)) {
    wlr_xwayland_surface_close(c->surface.xwayland);
    return;
  }
#endif
  wlr_xdg_toplevel_send_close(c->surface.xdg);
}

static inline uint32_t client_set_size(Client *c, uint32_t width,
                                       uint32_t height) {
#ifdef XWAYLAND
  if (client_is_x11(c)) {
    wlr_xwayland_surface_configure(c->surface.xwayland, c->geom.x, c->geom.y,
                                   width, height);
    return 0;
  }
#endif
  return wlr_xdg_toplevel_set_size(c->surface.xdg, width, height);
}

static inline struct wlr_surface *client_surface(Client *c) {
#ifdef XWAYLAND
  if (client_is_x11(c))
    return c->surface.xwayland->surface;
#endif
  return c->surface.xdg->surface;
}

static inline struct wlr_surface *
client_surface_at(Client *c, double cx, double cy, double *sx, double *sy) {
#ifdef XWAYLAND
  if (client_is_x11(c))
    return wlr_surface_surface_at(c->surface.xwayland->surface, cx, cy, sx, sy);
#endif
  return wlr_xdg_surface_surface_at(c->surface.xdg, cx, cy, sx, sy);
}

void applybounds(Client *c, struct wlr_box *bbox) {
  c->geom.width = MAX(1, c->geom.width);
  c->geom.height = MAX(1, c->geom.height);

  if (c->geom.x >= bbox->x + bbox->width)
    c->geom.x = bbox->x + bbox->width - c->geom.width;
  if (c->geom.y >= bbox->y + bbox->height)
    c->geom.y = bbox->y + bbox->height - c->geom.height;
  if (c->geom.x + c->geom.width <= bbox->x)
    c->geom.x = bbox->x;
  if (c->geom.y + c->geom.height <= bbox->y)
    c->geom.y = bbox->y;
}

void arrange(Monitor *m) {
  unsigned int i, n = 0, h, mw, my, ty, nmaster = 1;
  Client *c;

  wl_list_for_each(c, &clients, link) if (VISIBLEON(c, m)) n++;
  if (n == 0)
    return;

  mw = n > nmaster ? nmaster ? m->w.width * 0.5 : 0 : m->w.width;
  i = my = ty = 0;
  wl_list_for_each(c, &clients, link) {
    if (!VISIBLEON(c, m))
      continue;
    else if (i < nmaster) {
      h = (m->w.height - my) / (MIN(n, nmaster) - i);
      setsize(c, m->w.x, m->w.y + my, mw, h, 0);
      my += c->geom.height;
    } else {
      h = (m->w.height - ty) / (n - i);
      setsize(c, m->w.x + mw, m->w.y + ty, m->w.width - mw, h, 0);
      ty += c->geom.height;
    }
    i++;
  }
}

void arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area,
                  int exclusive) {
  LayerSurface *layersurface;
  struct wlr_box full_area = m->m;

  wl_list_for_each(layersurface, list, link) {
    struct wlr_layer_surface_v1 *wlr_layer_surface =
        layersurface->layer_surface;
    struct wlr_layer_surface_v1_state *state = &wlr_layer_surface->current;
    struct wlr_box bounds;
    struct wlr_box box = {.width = state->desired_width,
                          .height = state->desired_height};
    const uint32_t both_horiz =
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    const uint32_t both_vert =
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

    if (exclusive != (state->exclusive_zone > 0))
      continue;

    bounds = state->exclusive_zone == -1 ? full_area : *usable_area;

    // Horizontal axis
    if ((state->anchor & both_horiz) && box.width == 0) {
      box.x = bounds.x;
      box.width = bounds.width;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
      box.x = bounds.x;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
      box.x = bounds.x + (bounds.width - box.width);
    } else {
      box.x = bounds.x + ((bounds.width / 2) - (box.width / 2));
    }
    // Vertical axis
    if ((state->anchor & both_vert) && box.height == 0) {
      box.y = bounds.y;
      box.height = bounds.height;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
      box.y = bounds.y;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
      box.y = bounds.y + (bounds.height - box.height);
    } else {
      box.y = bounds.y + ((bounds.height / 2) - (box.height / 2));
    }
    // Margin
    if ((state->anchor & both_horiz) == both_horiz) {
      box.x += state->margin.left;
      box.width -= state->margin.left + state->margin.right;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
      box.x += state->margin.left;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
      box.x -= state->margin.right;
    }
    if ((state->anchor & both_vert) == both_vert) {
      box.y += state->margin.top;
      box.height -= state->margin.top + state->margin.bottom;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
      box.y += state->margin.top;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
      box.y -= state->margin.bottom;
    }
    if (box.width < 0 || box.height < 0) {
      wlr_layer_surface_v1_close(wlr_layer_surface);
      continue;
    }
    layersurface->geo = box;

    wlr_layer_surface_v1_configure(wlr_layer_surface, box.width, box.height);
  }
}

void arrangelayers(Monitor *m) {
  struct wlr_box usable_area = m->m;
  uint32_t layers_above_shell[] = {
      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      ZWLR_LAYER_SHELL_V1_LAYER_TOP,
  };
  size_t nlayers = LENGTH(layers_above_shell);
  LayerSurface *layersurface;
  struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);

  if (memcmp(&usable_area, &m->w, sizeof(struct wlr_box))) {
    m->w = usable_area;
    arrange(m);
  }

  // Arrange non-exlusive surfaces from top->bottom
  arrangelayer(m, &m->layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &usable_area,
               0);
  arrangelayer(m, &m->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &usable_area, 0);
  arrangelayer(m, &m->layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM], &usable_area,
               0);
  arrangelayer(m, &m->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND],
               &usable_area, 0);

  // Find topmost keyboard interactive layer, if such a layer exists
  for (size_t i = 0; i < nlayers; ++i) {
    wl_list_for_each_reverse(layersurface, &m->layers[layers_above_shell[i]],
                             link) {
      if (layersurface->layer_surface->current.keyboard_interactive &&
          layersurface->layer_surface->mapped) {
        // Deactivate the focused client.
        focusclient(NULL, 0);
        wlr_seat_keyboard_notify_enter(
            seat, layersurface->layer_surface->surface, kb->keycodes,
            kb->num_keycodes, &kb->modifiers);
        return;
      }
    }
  }
}

void axisnotify(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits an axis event,
   * for example when you move the scroll wheel. */
  struct wlr_event_pointer_axis *event = data;
  /* Notify the client with pointer focus of the axis event. */
  wlr_seat_pointer_notify_axis(seat, event->time_msec, event->orientation,
                               event->delta, event->delta_discrete,
                               event->source);
}

void buttonpress(struct wl_listener *listener, void *data) {
  struct wlr_event_pointer_button *event = data;
  struct wlr_keyboard *keyboard;
  uint32_t mods;

  switch (event->state) {
  case WLR_BUTTON_PRESSED:;
    keyboard = wlr_seat_get_keyboard(seat);
    mods = wlr_keyboard_get_modifiers(keyboard);
    if (mods == WLR_MODIFIER_LOGO) {
      if (event->button == BTN_LEFT) {
        move();
        return;
      }
    }
    break;
  case WLR_BUTTON_RELEASED:
    if (cursor_mode != CurNormal) {
      cursor_mode = CurNormal;
      /* Drop the window off on its new monitor */
      selmon = xytomon(cursor->x, cursor->y);
      setmon(grabc, selmon, 0);
      return;
    }
    break;
  }
  /* If the event wasn't handled by the compositor, notify the client with
   * pointer focus that a button press has occurred */
  wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
                                 event->state);
}

void cleanup(void) {
#ifdef XWAYLAND
  wlr_xwayland_destroy(xwayland);
#endif
  wl_display_destroy_clients(dpy);

  wlr_backend_destroy(backend);
  wlr_cursor_destroy(cursor);
  wlr_output_layout_destroy(output_layout);
  wlr_seat_destroy(seat);
  wl_display_destroy(dpy);
}

void cleanupkeyboard(struct wl_listener *listener, void *data) {
  struct wlr_input_device *device = data;
  Keyboard *kb = device->data;

  wl_list_remove(&kb->link);
  wl_list_remove(&kb->modifiers.link);
  wl_list_remove(&kb->key.link);
  wl_list_remove(&kb->destroy.link);
  free(kb);
}

void cleanupmon(struct wl_listener *listener, void *data) {
  struct wlr_output *wlr_output = data;
  Monitor *m = wlr_output->data;
  int nmons, i = 0;

  wl_list_remove(&m->destroy.link);
  wl_list_remove(&m->frame.link);
  wl_list_remove(&m->link);
  wlr_output_layout_remove(output_layout, m->wlr_output);
  updatemons();

  nmons = wl_list_length(&mons);
  do // don't switch to disabled mons
    selmon = wl_container_of(mons.prev, selmon, link);
  while (!selmon->wlr_output->enabled && i++ < nmons);
  focusclient(focustop(selmon), 1);
  closemon(m);
  free(m);
}

void closemon(Monitor *m) {
  // move closed monitor's clients to the focused one
  Client *c;

  wl_list_for_each(c, &clients, link) {
    if (c->geom.x > m->m.width)
      setsize(c, c->geom.x - m->w.width, c->geom.y, c->geom.width,
              c->geom.height, 0);
    if (c->mon == m)
      setmon(c, selmon, c->tags);
  }
}

void commitlayersurfacenotify(struct wl_listener *listener, void *data) {
  LayerSurface *layersurface =
      wl_container_of(listener, layersurface, surface_commit);
  struct wlr_layer_surface_v1 *wlr_layer_surface = layersurface->layer_surface;
  struct wlr_output *wlr_output = wlr_layer_surface->output;
  Monitor *m;

  if (!wlr_output)
    return;

  m = wlr_output->data;
  arrangelayers(m);

  if (layersurface->layer != wlr_layer_surface->current.layer) {
    wl_list_remove(&layersurface->link);
    wl_list_insert(&m->layers[wlr_layer_surface->current.layer],
                   &layersurface->link);
    layersurface->layer = wlr_layer_surface->current.layer;
  }
}

void commitnotify(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, commit);

  /* mark a pending resize as completed */
  if (c->resize && c->resize <= c->surface.xdg->configure_serial)
    c->resize = 0;
}

void createkeyboard(struct wlr_input_device *device) {
  struct xkb_context *context;
  struct xkb_keymap *keymap;
  Keyboard *kb = device->data = calloc(1, sizeof(*kb));
  kb->device = device;

  /* Prepare an XKB keymap and assign it to the keyboard. */
  context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  keymap = xkb_map_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

  wlr_keyboard_set_keymap(device->keyboard, keymap);
  xkb_keymap_unref(keymap);
  xkb_context_unref(context);
  wlr_keyboard_set_repeat_info(device->keyboard, 25, 220);

  /* Here we set up listeners for keyboard events. */
  LISTEN(&device->keyboard->events.modifiers, &kb->modifiers, keypressmod);
  LISTEN(&device->keyboard->events.key, &kb->key, keypress);
  LISTEN(&device->events.destroy, &kb->destroy, cleanupkeyboard);

  wlr_seat_set_keyboard(seat, device);

  /* And add the keyboard to our list of keyboards */
  wl_list_insert(&keyboards, &kb->link);
}

void createmon(struct wl_listener *listener, void *data) {
  /* This event is raised by the backend when a new output (aka a display or
   * monitor) becomes available. */
  struct wlr_output *wlr_output = data;
  const MonitorRule *r;
  size_t nlayers;
  Monitor *m, *moni, *insertmon = NULL;

  /* Allocates and configures monitor state using configured rules */
  m = wlr_output->data = calloc(1, sizeof(*m));
  m->wlr_output = wlr_output;
  m->tagset[0] = m->tagset[1] = 1;
  m->position = -1;
  for (r = monrules; r < END(monrules); r++) {
    if (!r->name || strstr(wlr_output->name, r->name)) {
      setmode(wlr_output, r->w, r->h, r->refresh);
      m->position = r - monrules;
      break;
    }
  }
  wlr_output_enable_adaptive_sync(wlr_output, 1);
  /* Set up event listeners */
  LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
  LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);

  wl_list_for_each(moni, &mons, link) if (m->position > moni->position)
      insertmon = moni;

  wl_list_insert(insertmon ? &insertmon->link : &mons, &m->link);

  wlr_output_enable(wlr_output, 1);
  if (!wlr_output_commit(wlr_output))
    return;

  /* Adds this to the output layout in the order it was configured in.
   *
   * The output layout utility automatically adds a wl_output global to the
   * display, which Wayland clients can see to find out information about the
   * output (such as DPI, scale factor, manufacturer, etc).
   */
  wlr_output_layout_add(output_layout, wlr_output, r->x, r->y);
  sgeom = *wlr_output_layout_get_box(output_layout, NULL);

  nlayers = LENGTH(m->layers);
  for (size_t i = 0; i < nlayers; ++i)
    wl_list_init(&m->layers[i]);

  /* When adding monitors, the geometries of all monitors must be updated */
  updatemons();
}

void createnotify(struct wl_listener *listener, void *data) {
  /* This event is raised when wlr_xdg_shell receives a new xdg surface from a
   * client, either a toplevel (application window) or popup. */
  struct wlr_xdg_surface *xdg_surface = data;
  Client *c;

  if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
    return;

  /* Allocate a Client for this surface */
  c = xdg_surface->data = calloc(1, sizeof(*c));
  c->surface.xdg = xdg_surface;

  /* Tell the client not to try anything fancy */
  wlr_xdg_toplevel_set_tiled(c->surface.xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
                                                 WLR_EDGE_LEFT |
                                                 WLR_EDGE_RIGHT);

  LISTEN(&xdg_surface->surface->events.commit, &c->commit, commitnotify);
  LISTEN(&xdg_surface->events.map, &c->map, mapnotify);
  LISTEN(&xdg_surface->events.unmap, &c->unmap, unmapnotify);
  LISTEN(&xdg_surface->events.destroy, &c->destroy, destroynotify);
}

void createlayersurface(struct wl_listener *listener, void *data) {
  struct wlr_layer_surface_v1 *wlr_layer_surface = data;
  LayerSurface *layersurface;
  Monitor *m;
  struct wlr_layer_surface_v1_state old_state;

  if (!wlr_layer_surface->output) {
    wlr_layer_surface->output = selmon->wlr_output;
  }

  layersurface = calloc(1, sizeof(LayerSurface));
  LISTEN(&wlr_layer_surface->surface->events.commit,
         &layersurface->surface_commit, commitlayersurfacenotify);
  LISTEN(&wlr_layer_surface->events.destroy, &layersurface->destroy,
         destroylayersurfacenotify);
  LISTEN(&wlr_layer_surface->events.map, &layersurface->map,
         maplayersurfacenotify);
  LISTEN(&wlr_layer_surface->events.unmap, &layersurface->unmap,
         unmaplayersurfacenotify);

  layersurface->layer_surface = wlr_layer_surface;
  wlr_layer_surface->data = layersurface;

  m = wlr_layer_surface->output->data;
  wl_list_insert(&m->layers[wlr_layer_surface->client_pending.layer],
                 &layersurface->link);

  // Temporarily set the layer's current state to client_pending
  // so that we can easily arrange it
  old_state = wlr_layer_surface->current;
  wlr_layer_surface->current = wlr_layer_surface->client_pending;
  arrangelayers(m);
  wlr_layer_surface->current = old_state;
}

void createpointer(struct wlr_input_device *device) {
  wlr_cursor_attach_input_device(cursor, device);
}

void cursorframe(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits an frame
   * event. Frame events are sent after regular pointer events to group
   * multiple events together. For instance, two axis events may happen at the
   * same time, in which case a frame event won't be sent in between. */
  /* Notify the client with pointer focus of the frame event. */
  wlr_seat_pointer_notify_frame(seat);
}

void destroylayersurfacenotify(struct wl_listener *listener, void *data) {
  LayerSurface *layersurface = wl_container_of(listener, layersurface, destroy);

  if (layersurface->layer_surface->mapped)
    unmaplayersurface(layersurface);
  wl_list_remove(&layersurface->link);
  wl_list_remove(&layersurface->destroy.link);
  wl_list_remove(&layersurface->map.link);
  wl_list_remove(&layersurface->unmap.link);
  wl_list_remove(&layersurface->surface_commit.link);
  if (layersurface->layer_surface->output) {
    Monitor *m = layersurface->layer_surface->output->data;
    if (m)
      arrangelayers(m);
    layersurface->layer_surface->output = NULL;
  }
  free(layersurface);
}

void destroynotify(struct wl_listener *listener, void *data) {
  /* Called when the surface is destroyed and should never be shown again. */
  Client *c = wl_container_of(listener, c, destroy);
  wl_list_remove(&c->map.link);
  wl_list_remove(&c->unmap.link);
  wl_list_remove(&c->destroy.link);
#ifdef XWAYLAND
  if (c->type == X11Managed)
    wl_list_remove(&c->activate.link);
  else if (c->type == XDGShell)
#endif
    wl_list_remove(&c->commit.link);
  free(c);
}

Monitor *dirtomon(int dir) {
  Monitor *m;

  if (dir > 0) {
    if (selmon->link.next == &mons)
      return wl_container_of(mons.next, m, link);
    return wl_container_of(selmon->link.next, m, link);
  } else {
    if (selmon->link.prev == &mons)
      return wl_container_of(mons.prev, m, link);
    return wl_container_of(selmon->link.prev, m, link);
  }
}

void focusclient(Client *c, int lift) {
  struct wlr_surface *old = seat->keyboard_state.focused_surface;
  struct wlr_keyboard *kb;

  /* Raise client in stacking order if requested */
  if (c && lift) {
    wl_list_remove(&c->slink);
    wl_list_insert(&stack, &c->slink);
  }

  if (c && client_surface(c) == old)
    return;

  /* Put the new client atop the focus stack and select its monitor */
  if (c) {
    wl_list_remove(&c->flink);
    wl_list_insert(&fstack, &c->flink);
    selmon = c->mon;
  }

  /* Deactivate old client if focus is changing */
  if (old && (!c || client_surface(c) != old)) {
    /* If an overlay is focused, don't focus or activate the client,
     * but only update its position in fstack to render its border with
     * focuscolor and focus it after the overlay is closed. It's probably
     * pointless to check if old is a layer surface since it can't be anything
     * else at this point. */
    if (wlr_surface_is_layer_surface(old)) {
      struct wlr_layer_surface_v1 *wlr_layer_surface =
          wlr_layer_surface_v1_from_wlr_surface(old);

      if (wlr_layer_surface->mapped &&
          (wlr_layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
           wlr_layer_surface->current.layer ==
               ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY))
        return;
    } else {
      client_activate_surface(old, 0);
    }
  }

  if (!c) {
    /* With no client, all we have left is to clear focus */
    wlr_seat_keyboard_notify_clear_focus(seat);
    return;
  }

  /* Have a client, so focus its top-level wlr_surface */
  kb = wlr_seat_get_keyboard(seat);
  wlr_seat_keyboard_notify_enter(seat, client_surface(c), kb->keycodes,
                                 kb->num_keycodes, &kb->modifiers);

  /* Activate the new client */
  client_activate_surface(client_surface(c), 1);
}

void focusmon(const int dir) {
  do
    selmon = dirtomon(dir);
  while (!selmon->wlr_output->enabled);
  focusclient(focustop(selmon), 1);
}

void focusstack(const int dir) {
  /* Focus the next or previous client (in tiling order) on selmon */
  Client *c, *sel = selclient();
  if (!sel)
    return;
  if (dir > 0) {
    wl_list_for_each(c, &sel->link, link) {
      if (&c->link == &clients)
        continue; /* wrap past the sentinel node */
      if (VISIBLEON(c, selmon))
        break; /* found it */
    }
  } else {
    wl_list_for_each_reverse(c, &sel->link, link) {
      if (&c->link == &clients)
        continue; /* wrap past the sentinel node */
      if (VISIBLEON(c, selmon))
        break; /* found it */
    }
  }
  /* If only one client is visible on selmon, then c == sel */
  focusclient(c, 1);
}

Client *focustop(Monitor *m) {
  Client *c;
  wl_list_for_each(c, &fstack, flink) if (VISIBLEON(c, m)) return c;
  return NULL;
}

/* This event is raised by the backend when a new input device becomes
 * available. */
void inputdevice(struct wl_listener *listener, void *data) {
  struct wlr_input_device *device = data;

  if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
    createkeyboard(device);
  } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
    createpointer(device);
  }

  wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER |
                                      WL_SEAT_CAPABILITY_KEYBOARD);
}

int keybinding(uint32_t mods, xkb_keysym_t sym) {
  if (mods == WLR_MODIFIER_LOGO) {
    switch (sym) {
      CASE(XKB_KEY_Return, spawn("bemenu-run"))
      CASE(XKB_KEY_p, spawn("passmenu"))
      CASE(XKB_KEY_space, zoom())
      CASE(XKB_KEY_c, focusstack(1))
      CASE(XKB_KEY_h, focusstack(-1))
      CASE(XKB_KEY_s, focusmon(1))
      CASE(XKB_KEY_t, focusmon(-1))
      CASE(XKB_KEY_i, view(1))
      CASE(XKB_KEY_e, view(2))
      CASE(XKB_KEY_o, view(4))
      CASE(XKB_KEY_n, view(8))
    }
  } else if (mods == (WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL)) {
    switch (sym) {
      CASE(XKB_KEY_Return, spawn("alacritty"))
      CASE(XKB_KEY_c, killclient())
      CASE(XKB_KEY_s, tagmon(1))
      CASE(XKB_KEY_t, tagmon(-1))
      CASE(XKB_KEY_i, tag(1))
      CASE(XKB_KEY_e, tag(2))
      CASE(XKB_KEY_o, tag(4))
      CASE(XKB_KEY_n, tag(8))
    }
  }
  return 0;
}

void keypress(struct wl_listener *listener, void *data) {
  /* This event is raised when a key is pressed or released. */
  Keyboard *kb = wl_container_of(listener, kb, key);
  struct wlr_event_keyboard_key *event = data;

  /* Translate libinput keycode -> xkbcommon */
  uint32_t keycode = event->keycode + 8;
  /* Get a list of keysyms based on the keymap for this keyboard */
  const xkb_keysym_t *syms;
  int nsyms =
      xkb_state_key_get_syms(kb->device->keyboard->xkb_state, keycode, &syms);

  int handled = 0;
  uint32_t mods = wlr_keyboard_get_modifiers(kb->device->keyboard);

  /* On _press_, attempt to process a compositor keybinding. */
  if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
    for (int i = 0; i < nsyms; i++)
      handled = keybinding(mods, syms[i]) || handled;

  if (!handled) {
    /* Pass unhandled keycodes along to the client. */
    wlr_seat_set_keyboard(seat, kb->device);
    wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
                                 event->state);
  }
}

void keypressmod(struct wl_listener *listener, void *data) {
  /* This event is raised when a modifier key, such as shift or alt, is
   * pressed. We simply communicate this to the client. */
  Keyboard *kb = wl_container_of(listener, kb, modifiers);
  /*
   * A seat can only have one keyboard, but this is a limitation of the
   * Wayland protocol - not wlroots. We assign all connected keyboards to the
   * same seat. You can swap out the underlying wlr_keyboard like this and
   * wlr_seat handles this transparently.
   */
  wlr_seat_set_keyboard(seat, kb->device);
  /* Send modifiers to the client. */
  wlr_seat_keyboard_notify_modifiers(seat, &kb->device->keyboard->modifiers);
}

void killclient() {
  onselected() { client_send_close(sel); }
}

void maplayersurfacenotify(struct wl_listener *listener, void *data) {
  LayerSurface *layersurface = wl_container_of(listener, layersurface, map);
  wlr_surface_send_enter(layersurface->layer_surface->surface,
                         layersurface->layer_surface->output);
  motionnotify(0);
}

void mapnotify(struct wl_listener *listener, void *data) {
  /* Called when the surface is mapped, or ready to display on-screen. */
  Client *c = wl_container_of(listener, c, map);

  if (client_is_unmanaged(c)) {
    /* Insert this independent into independents lists. */
    wl_list_insert(&independents, &c->link);
    return;
  }

  /* Insert this client into client lists. */
  wl_list_insert(&clients, &c->link);
  wl_list_insert(&fstack, &c->flink);
  wl_list_insert(&stack, &c->slink);

  client_get_geometry(c, &c->geom);
  setmon(c, selmon, 0);
}

void motionnotify(uint32_t time) {
  double sx = 0, sy = 0;
  struct wlr_surface *surface = NULL;
  Client *c = NULL;

  // time is 0 in internal calls meant to restore pointer focus.
  if (time) {
    /* Update selmon (even while dragging a window) */
    selmon = xytomon(cursor->x, cursor->y);
  }

  /* If we are currently grabbing the mouse, handle and return */
  if (cursor_mode == CurMove) {
    /* Move the grabbed client to the new position. */
    setsize(grabc, cursor->x - grabcx, cursor->y - grabcy, grabc->geom.width,
            grabc->geom.height, 1);
    return;
  }

  if ((surface =
           xytolayersurface(&selmon->layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
                            cursor->x, cursor->y, &sx, &sy)))
    ;
  else if ((surface =
                xytolayersurface(&selmon->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP],
                                 cursor->x, cursor->y, &sx, &sy)))
    ;
#ifdef XWAYLAND
  /* Find an independent under the pointer and send the event along. */
  else if ((c = xytoindependent(cursor->x, cursor->y))) {
    surface = wlr_surface_surface_at(
        c->surface.xwayland->surface, cursor->x - c->surface.xwayland->x,
        cursor->y - c->surface.xwayland->y, &sx, &sy);

    /* Otherwise, find the client under the pointer and send the event along.
     */
  }
#endif
  else if ((c = xytoclient(cursor->x, cursor->y))) {
    surface = client_surface_at(c, cursor->x - c->geom.x, cursor->y - c->geom.y,
                                &sx, &sy);
  } else if ((surface = xytolayersurface(
                  &selmon->layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM], cursor->x,
                  cursor->y, &sx, &sy)))
    ;
  else
    surface =
        xytolayersurface(&selmon->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND],
                         cursor->x, cursor->y, &sx, &sy);

  /* If there's no client surface under the cursor, set the cursor image to a
   * default. This is what makes the cursor image appear when you move it
   * off of a client or over its border. */
  pointerfocus(c, surface, sx, sy, time);
}

void motionrelative(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits a _relative_
   * pointer motion event (i.e. a delta) */
  struct wlr_event_pointer_motion *event = data;
  /* The cursor doesn't move unless we tell it to. The cursor automatically
   * handles constraining the motion to the output layout, as well as any
   * special configuration applied for the specific input device which
   * generated the event. You can pass NULL for the device if you want to move
   * the cursor around without any input. */
  wlr_cursor_move(cursor, event->device, event->delta_x, event->delta_y);
  motionnotify(event->time_msec);
}

void move() {
  grabc = xytoclient(cursor->x, cursor->y);
  if (!grabc)
    return;

  cursor_mode = CurMove;
  grabcx = cursor->x - grabc->geom.x;
  grabcy = cursor->y - grabc->geom.y;
}

void pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
                  uint32_t time) {
  /* Use top level surface if nothing more specific given */
  if (c && !surface)
    surface = client_surface(c);

  /* If surface is NULL, clear pointer focus */
  if (!surface) {
    wlr_seat_pointer_notify_clear_focus(seat);
    return;
  }

  /* If surface is already focused, only notify of motion */
  if (surface == seat->pointer_state.focused_surface) {
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    return;
  }

  /* Otherwise, let the client know that the mouse cursor has entered one
   * of its surfaces, and make keyboard focus follow if desired. */
  wlr_seat_pointer_notify_enter(seat, surface, sx, sy);

  if (!c || client_is_unmanaged(c))
    return;

  focusclient(c, 0);
}

void render(struct wlr_surface *surface, int sx, int sy, void *data) {
  /* This function is called for every surface that needs to be rendered. */
  struct render_data *rdata = data;
  struct wlr_output *output = rdata->output;
  double ox = 0, oy = 0;

  /* We first obtain a wlr_texture, which is a GPU resource. wlroots
   * automatically handles negotiating these with the client. The underlying
   * resource could be an opaque handle passed from the client, or the client
   * could have sent a pixel buffer which we copied to the GPU, or a few other
   * means. You don't have to worry about this, wlroots takes care of it. */
  struct wlr_texture *texture = wlr_surface_get_texture(surface);
  if (!texture)
    return;

  wlr_output_layout_output_coords(output_layout, output, &ox, &oy);
  wlr_render_texture(drw, texture, output->transform_matrix, ox + rdata->x + sx,
                     oy + rdata->y + sy, 1);
  wlr_surface_send_frame_done(surface, rdata->when);
}

void renderclients(Monitor *m, struct timespec *now) {
  Client *c;
  double ox, oy;
  struct render_data rdata;
  /* Each subsequent window we render is rendered on top of the last. Because
   * our stacking list is ordered front-to-back, we iterate over it backwards.
   */
  wl_list_for_each_reverse(c, &stack, slink) {
    /* Only render visible clients which show on this monitor */
    if (!VISIBLEON(c, c->mon) ||
        !wlr_output_layout_intersects(output_layout, m->wlr_output, &c->geom))
      continue;

    ox = c->geom.x, oy = c->geom.y;
    wlr_output_layout_output_coords(output_layout, m->wlr_output, &ox, &oy);

    /* This calls our render function for each surface among the
     * xdg_surface's toplevel and popups. */
    rdata.output = m->wlr_output;
    rdata.when = now;
    rdata.x = c->geom.x;
    rdata.y = c->geom.y;
    client_for_each_surface(c, render, &rdata);
  }
}

void renderlayer(struct wl_list *layer_surfaces, struct timespec *now) {
  LayerSurface *layersurface;
  wl_list_for_each(layersurface, layer_surfaces, link) {
    struct render_data rdata = {
        .output = layersurface->layer_surface->output,
        .when = now,
        .x = layersurface->geo.x,
        .y = layersurface->geo.y,
    };

    wlr_surface_for_each_surface(layersurface->layer_surface->surface, render,
                                 &rdata);
  }
}

/* This function is called every time an output is ready to display a frame */
void rendermon(struct wl_listener *listener, void *data) {
  Monitor *m = wl_container_of(listener, m, frame);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  /* wlr_output_attach_render makes the OpenGL context current. */
  if (!wlr_output_attach_render(m->wlr_output, NULL))
    return;

  wlr_renderer_begin(drw, m->wlr_output->width, m->wlr_output->height);
  wlr_renderer_clear(drw, (float[]){0.0, 0.0, 0.0, 1.0});

  renderlayer(&m->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND], &now);
  renderlayer(&m->layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM], &now);
  renderclients(m, &now);
#ifdef XWAYLAND
  renderindependents(m->wlr_output, &now);
#endif
  renderlayer(&m->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &now);
  renderlayer(&m->layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &now);
  wlr_renderer_end(drw);

  wlr_output_commit(m->wlr_output);
}

void setsize(Client *c, int x, int y, int w, int h, int interact) {
  struct wlr_box *bbox = interact ? &sgeom : &c->mon->w;
  c->geom.x = x;
  c->geom.y = y;
  c->geom.width = w;
  c->geom.height = h;
  applybounds(c, bbox);
  /* wlroots makes this a no-op if size hasn't changed */
  c->resize = client_set_size(c, c->geom.width, c->geom.height);
}

void run() {
  /* Add a Unix socket to the Wayland display. */
  const char *socket = wl_display_add_socket_auto(dpy);
  if (!socket)
    BARF("%s", "startup: display_add_socket_auto");

  /* Start the backend. This will enumerate outputs and inputs, become the DRM
   * master, etc */
  if (!wlr_backend_start(backend))
    BARF("%s", "startup: backend_start");

  /* Now that outputs are initialized, choose initial selmon based on
   * cursor position, and set default cursor image */
  selmon = xytomon(cursor->x, cursor->y);

  setenv("WAYLAND_DISPLAY", socket, 1);

  wl_display_run(dpy);
}

Client *selclient(void) {
  Client *c = wl_container_of(fstack.next, c, flink);
  if (wl_list_empty(&fstack) || !VISIBLEON(c, selmon))
    return NULL;
  return c;
}

void setcursor(struct wl_listener *listener, void *data) {
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  if (cursor_mode != CurNormal)
    return;

  if (event->seat_client == seat->pointer_state.focused_client)
    wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x,
                           event->hotspot_y);
}

void setmode(struct wlr_output *output, int w, int h, int refresh) {
  struct wlr_output_mode *mode = NULL;
  wl_list_for_each(mode, &output->modes, link) {
    if (mode->width == w && mode->height == h && mode->refresh == refresh) {
      wlr_output_set_mode(output, mode);
      return;
    }
  }
  wlr_output_set_mode(output, wlr_output_preferred_mode(output));
}

void setmon(Client *c, Monitor *m, unsigned int newtags) {
  if (c->mon == m) {
    return;
  }

  Monitor *oldmon = c->mon;

  c->mon = m;

  /* TODO leave/enter is not optimal but works */
  if (oldmon) {
    wlr_surface_send_leave(client_surface(c), oldmon->wlr_output);
    arrange(oldmon);
  }
  if (m) {
    /* Make sure window actually overlaps with the monitor */
    applybounds(c, &m->m);
    wlr_surface_send_enter(client_surface(c), m->wlr_output);
    c->tags = newtags
                  ? newtags
                  : m->tagset[m->seltags]; /* assign tags of target monitor */
    arrange(m);
  }
  focusclient(focustop(selmon), 1);
}

/* This event is raised by the seat when a client wants to set the
 * selection, usually when the user copies something. wlroots allows
 * compositors to ignore such requests if they so choose, but in dwl we
 * always honor
 */
void setpsel(struct wl_listener *listener, void *data) {
  struct wlr_seat_request_set_primary_selection_event *event = data;
  wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void setsel(struct wl_listener *listener, void *data) {
  struct wlr_seat_request_set_selection_event *event = data;
  wlr_seat_set_selection(seat, event->source, event->serial);
}

void setup(void) {
  /* The Wayland display is managed by libwayland. It handles accepting
   * clients from the Unix socket, manging Wayland globals, and so on. */
  dpy = wl_display_create();

  /* clean up child processes immediately */
  sigchld(0);

  /* The backend is a wlroots feature which abstracts the underlying input and
   * output hardware. The autocreate option will choose the most suitable
   * backend based on the current environment, such as opening an X11 window
   * if an X11 server is running. The NULL argument here optionally allows you
   * to pass in a custom renderer if wlr_renderer doesn't meet your needs. The
   * backend uses the renderer, for example, to fall back to software cursors
   * if the backend does not support hardware cursors (some older GPUs
   * don't). */
  if (!(backend = wlr_backend_autocreate(dpy)))
    BARF("%s", "couldn't create backend");

  /* If we don't provide a renderer, autocreate makes a GLES2 renderer for us.
   * The renderer is responsible for defining the various pixel formats it
   * supports for shared memory, this configures that for clients. */
  drw = wlr_backend_get_renderer(backend);
  wlr_renderer_init_wl_display(drw, dpy);

  /* This creates some hands-off wlroots interfaces. The compositor is
   * necessary for clients to allocate surfaces and the data device manager
   * handles the clipboard. Each of these wlroots interfaces has room for you
   * to dig your fingers in and play with their behavior if you want. Note
   * that the clients cannot set the selection directly without compositor
   * approval, see the setsel() function. */
  compositor = wlr_compositor_create(dpy, drw);
  wlr_export_dmabuf_manager_v1_create(dpy);
  wlr_screencopy_manager_v1_create(dpy);
  wlr_data_control_manager_v1_create(dpy);
  wlr_data_device_manager_create(dpy);
  wlr_gamma_control_manager_v1_create(dpy);
  wlr_primary_selection_v1_device_manager_create(dpy);
  wlr_viewporter_create(dpy);

  /* Creates an output layout, which a wlroots utility for working with an
   * arrangement of screens in a physical layout. */
  output_layout = wlr_output_layout_create();
  wlr_xdg_output_manager_v1_create(dpy, output_layout);

  /* Configure a listener to be notified when new outputs are available on the
   * backend. */
  wl_list_init(&mons);
  wl_signal_add(&backend->events.new_output, &new_output);

  /* Set up our client lists and the xdg-shell. The xdg-shell is a
   * Wayland protocol which is used for application windows. For more
   * detail on shells, refer to the article:
   *
   * https://drewdevault.com/2018/07/29/Wayland-shells.html
   */
  wl_list_init(&clients);
  wl_list_init(&fstack);
  wl_list_init(&stack);
  wl_list_init(&independents);

  layer_shell = wlr_layer_shell_v1_create(dpy);
  wl_signal_add(&layer_shell->events.new_surface, &new_layer_shell_surface);

  xdg_shell = wlr_xdg_shell_create(dpy);
  wl_signal_add(&xdg_shell->events.new_surface, &new_xdg_surface);

  /*
   * Creates a cursor, which is a wlroots utility for tracking the cursor
   * image shown on screen.
   */
  cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(cursor, output_layout);

  wl_signal_add(&cursor->events.motion, &cursor_motion);
  wl_signal_add(&cursor->events.button, &cursor_button);
  wl_signal_add(&cursor->events.axis, &cursor_axis);
  wl_signal_add(&cursor->events.frame, &cursor_frame);

  wl_list_init(&keyboards);
  wl_signal_add(&backend->events.new_input, &new_input);
  seat = wlr_seat_create(dpy, "seat0");
  wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
  wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
  wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);

  output_mgr = wlr_output_manager_v1_create(dpy);
  wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);

#ifdef XWAYLAND
  /*
   * Initialise the XWayland X server.
   * It will be started when the first X client is started.
   */
  xwayland = wlr_xwayland_create(dpy, compositor, 1);
  if (xwayland) {
    wl_signal_add(&xwayland->events.ready, &xwayland_ready);
    wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);
    setenv("DISPLAY", xwayland->display_name, 1);
  } else {
    fprintf(stderr,
            "failed to setup XWayland X server, continuing without it\n");
  }
#endif
}

void sigchld(int unused) {
  if (signal(SIGCHLD, sigchld) == SIG_ERR)
    EBARF("%s", "can't install SIGCHLD handler");
  while (0 < waitpid(-1, NULL, WNOHANG))
    ;
}

void spawn(const char *cmd) {
  if (fork() == 0) {
    setsid();
    execvp(cmd, (char *[]){NULL});
  }
}

void tag(const unsigned int tag) {
  Client *sel = selclient();
  if (sel && tag & TAGMASK) {
    sel->tags = tag & TAGMASK;
    focusclient(focustop(selmon), 1);
    arrange(selmon);
  }
}

void tagmon(const unsigned int tag) {
  onselected() { setmon(sel, dirtomon(tag), 0); }
}

void unmaplayersurface(LayerSurface *layersurface) {
  layersurface->layer_surface->mapped = 0;
  if (layersurface->layer_surface->surface ==
      seat->keyboard_state.focused_surface)
    focusclient(selclient(), 1);
  motionnotify(0);
}

void unmaplayersurfacenotify(struct wl_listener *listener, void *data) {
  LayerSurface *layersurface = wl_container_of(listener, layersurface, unmap);
  unmaplayersurface(layersurface);
}

void unmapnotify(struct wl_listener *listener, void *data) {
  /* Called when the surface is unmapped, and should no longer be shown. */
  Client *c = wl_container_of(listener, c, unmap);
  wl_list_remove(&c->link);
  if (client_is_unmanaged(c))
    return;

  setmon(c, NULL, 0);
  wl_list_remove(&c->flink);
  wl_list_remove(&c->slink);
}

void updatemons() {
  struct wlr_output_configuration_v1 *config =
      wlr_output_configuration_v1_create();
  Monitor *m;
  sgeom = *wlr_output_layout_get_box(output_layout, NULL);
  wl_list_for_each(m, &mons, link) {
    struct wlr_output_configuration_head_v1 *config_head =
        wlr_output_configuration_head_v1_create(config, m->wlr_output);

    /* Get the effective monitor geometry to use for surfaces */
    m->m = m->w = *wlr_output_layout_get_box(output_layout, m->wlr_output);
    /* Calculate the effective monitor geometry to use for clients */
    arrangelayers(m);
    /* Don't move clients to the left output when plugging monitors */
    arrange(m);

    config_head->state.enabled = m->wlr_output->enabled;
    config_head->state.mode = m->wlr_output->current_mode;
    config_head->state.x = m->m.x;
    config_head->state.y = m->m.y;
  }

  wlr_output_manager_v1_set_configuration(output_mgr, config);
}

void view(const unsigned int tag) {
  if ((tag & TAGMASK) == selmon->tagset[selmon->seltags])
    return;
  selmon->seltags ^= 1; /* toggle sel tagset */
  if (tag & TAGMASK)
    selmon->tagset[selmon->seltags] = tag & TAGMASK;
  focusclient(focustop(selmon), 1);
  arrange(selmon);
}

Client *xytoclient(double x, double y) {
  /* Find the topmost visible client (if any) at point (x, y), including
   * borders. This relies on stack being ordered from top to bottom. */
  Client *c;
  wl_list_for_each(c, &stack, slink) {
    if (VISIBLEON(c, c->mon) && wlr_box_contains_point(&c->geom, x, y))
      return c;
  }
  return NULL;
}

struct wlr_surface *xytolayersurface(struct wl_list *layer_surfaces, double x,
                                     double y, double *sx, double *sy) {
  LayerSurface *layersurface;
  wl_list_for_each_reverse(layersurface, layer_surfaces, link) {
    struct wlr_surface *sub;
    if (!layersurface->layer_surface->mapped)
      continue;
    sub = wlr_layer_surface_v1_surface_at(layersurface->layer_surface,
                                          x - layersurface->geo.x,
                                          y - layersurface->geo.y, sx, sy);
    if (sub)
      return sub;
  }
  return NULL;
}

Monitor *xytomon(double x, double y) {
  struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
  return o ? o->data : NULL;
}

void zoom() {
  Client *sel = selclient();
  if (sel) {
    wl_list_remove(&sel->link);
    wl_list_insert(&clients, &sel->link);
    focusclient(sel, 1);
    arrange(selmon);
  }
}

#ifdef XWAYLAND
void activatex11(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, activate);

  /* Only "managed" windows can be activated */
  if (c->type == X11Managed)
    wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

void configurex11(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, configure);
  struct wlr_xwayland_surface_configure_event *event = data;
  wlr_xwayland_surface_configure(c->surface.xwayland, event->x, event->y,
                                 event->width, event->height);
}

void createnotifyx11(struct wl_listener *listener, void *data) {
  struct wlr_xwayland_surface *xwayland_surface = data;
  Client *c;

  /* Allocate a Client for this surface */
  c = xwayland_surface->data = calloc(1, sizeof(*c));
  c->surface.xwayland = xwayland_surface;
  c->type = xwayland_surface->override_redirect ? X11Unmanaged : X11Managed;

  /* Listen to the various events it can emit */
  LISTEN(&xwayland_surface->events.map, &c->map, mapnotify);
  LISTEN(&xwayland_surface->events.unmap, &c->unmap, unmapnotify);
  LISTEN(&xwayland_surface->events.request_activate, &c->activate, activatex11);
  LISTEN(&xwayland_surface->events.request_configure, &c->configure,
         configurex11);
  LISTEN(&xwayland_surface->events.destroy, &c->destroy, destroynotify);
}

void renderindependents(struct wlr_output *output, struct timespec *now) {
  Client *c;
  struct render_data rdata;
  struct wlr_box geom;

  wl_list_for_each_reverse(c, &independents, link) {
    geom.x = c->surface.xwayland->x;
    geom.y = c->surface.xwayland->y;
    geom.width = c->surface.xwayland->width;
    geom.height = c->surface.xwayland->height;

    /* Only render visible clients which show on this output */
    if (!wlr_output_layout_intersects(output_layout, output, &geom))
      continue;

    rdata.output = output;
    rdata.when = now;
    rdata.x = c->surface.xwayland->x;
    rdata.y = c->surface.xwayland->y;
    wlr_surface_for_each_surface(c->surface.xwayland->surface, render, &rdata);
  }
}

void xwaylandready(struct wl_listener *listener, void *data) {
  xcb_connection_t *xc = xcb_connect(xwayland->display_name, NULL);
  int err = xcb_connection_has_error(xc);
  if (err) {
    return;
  }

  wlr_xwayland_set_seat(xwayland, seat);
  xcb_disconnect(xc);
}

Client *xytoindependent(double x, double y) {
  /* Find the topmost visible independent at point (x, y).
   * For independents, the most recently created can be used as the "top".
   * We rely on the X11 convention of unmapping unmanaged when the "owning"
   * client loses focus, which ensures that unmanaged are only visible on
   * the current tag. */
  Client *c;
  wl_list_for_each_reverse(c, &independents, link) {
    struct wlr_box geom = {
        .x = c->surface.xwayland->x,
        .y = c->surface.xwayland->y,
        .width = c->surface.xwayland->width,
        .height = c->surface.xwayland->height,
    };
    if (wlr_box_contains_point(&geom, x, y))
      return c;
  }
  return NULL;
}
#endif

int main(int argc, char *argv[]) {
  if (!getenv("XDG_RUNTIME_DIR"))
    BARF("%s", "XDG_RUNTIME_DIR must be set");
  setup();
  run();
  cleanup();
  return EXIT_SUCCESS;
}

void outputmgrapply(struct wl_listener *listener, void *data) {
  struct wlr_output_configuration_v1 *config = data;
  struct wlr_output_configuration_head_v1 *config_head;
  int ok = 1;

  wl_list_for_each(config_head, &config->heads, link) {
    struct wlr_output *wlr_output = config_head->state.output;

    wlr_output_enable(wlr_output, config_head->state.enabled);
    if (config_head->state.enabled) {
      if (config_head->state.mode)
        wlr_output_set_mode(wlr_output, config_head->state.mode);
      else
        wlr_output_set_custom_mode(wlr_output,
                                   config_head->state.custom_mode.width,
                                   config_head->state.custom_mode.height,
                                   config_head->state.custom_mode.refresh);

      wlr_output_layout_move(output_layout, wlr_output, config_head->state.x,
                             config_head->state.y);
    } else if (wl_list_length(&mons) > 1) {
      Monitor *m;
      wl_list_for_each(m, &mons, link) {
        if (m->wlr_output->name == wlr_output->name) {
          // focus the left monitor (relative to the current focus)
          m->wlr_output->enabled = !m->wlr_output->enabled;
          focusmon(-1);
          closemon(m);
          m->wlr_output->enabled = !m->wlr_output->enabled;
        }
      }
    }

    ok &= wlr_output_commit(wlr_output);
  }
  if (ok) {
    wlr_output_configuration_v1_send_succeeded(config);
    updatemons();
  } else {
    wlr_output_configuration_v1_send_failed(config);
  }
  wlr_output_configuration_v1_destroy(config);
}
