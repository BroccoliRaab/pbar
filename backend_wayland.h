#ifndef BACKEND_WAYLAND_H
#define BACKEND_WAYLAND_H

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <dbus/dbus.h>
#include <errno.h>

/* --- Backend Private State --- */

typedef struct {
    struct wl_surface *wl_surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *wl_buffer;
    struct wl_shm_pool *shm_pool;
    bool configured;
} wayland_state_t;

typedef struct sni_item_s {
    char *service;
    char *path;
    int width;
    int height;
    uint32_t *pixels; /* ARGB8888 Pixel Buffer */
    struct sni_item_s *next;
} sni_item_t;

static struct wl_display *wl_disp = NULL;
static struct wl_registry *wl_reg = NULL;
static struct wl_compositor *wl_comp = NULL;
static struct wl_shm *wl_shm_inst = NULL;
static struct zwlr_layer_shell_v1 *layer_shell = NULL;

static DBusConnection *dbus_conn = NULL;
static sni_item_t *sni_items = NULL;

static void wayland_update(pbar_output_t *out);

/* --- StatusNotifierItem (SNI) D-Bus Logic --- */

static void fetch_sni_icon(sni_item_t *item) {
    if (!dbus_conn || !item->service || !item->path) return;

    DBusMessage *msg = dbus_message_new_method_call(
        item->service, item->path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return;

    const char *iface = "org.kde.StatusNotifierItem";
    const char *prop = "IconPixmap";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, 1000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        dbus_error_free(&err);
        return;
    }

    DBusMessageIter iter, variant_iter, array_iter, struct_iter, bytes_iter;
    dbus_message_iter_init(reply, &iter);

    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&iter, &variant_iter);
        if (dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&variant_iter, &array_iter);

            /* Extract first icon pixmap struct (iiay) */
            if (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_STRUCT) {
                dbus_message_iter_recurse(&array_iter, &struct_iter);

                int w = 0, h = 0;
                dbus_message_iter_get_basic(&struct_iter, &w);
                dbus_message_iter_next(&struct_iter);
                dbus_message_iter_get_basic(&struct_iter, &h);
                dbus_message_iter_next(&struct_iter);

                if (w > 0 && h > 0 && dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&struct_iter, &bytes_iter);
                    int len = 0;
                    const uint8_t *raw_bytes = NULL;
                    dbus_message_iter_get_fixed_array(&bytes_iter, &raw_bytes, &len);

                    if (raw_bytes && len >= (w * h * 4)) {
                        free(item->pixels);
                        item->width = w;
                        item->height = h;
                        item->pixels = malloc(w * h * sizeof(uint32_t));

                        /* Convert ARGB Big-Endian network byte order to host ARGB8888 */
                        for (int i = 0; i < w * h; i++) {
                            uint8_t a = raw_bytes[i * 4 + 0];
                            uint8_t r = raw_bytes[i * 4 + 1];
                            uint8_t g = raw_bytes[i * 4 + 2];
                            uint8_t b = raw_bytes[i * 4 + 3];
                            item->pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                        }
                    }
                }
            }
        }
    }
    dbus_message_unref(reply);
}

static void add_sni_item(const char *service_arg) {
    char service[256] = {0};
    char path[256] = "/StatusNotifierItem";

    if (service_arg[0] == '/') {
        strncpy(path, service_arg, sizeof(path) - 1);
    } else {
        const char *slash = strchr(service_arg, '/');
        if (slash) {
            size_t s_len = slash - service_arg;
            if (s_len >= sizeof(service)) s_len = sizeof(service) - 1;
            strncpy(service, service_arg, s_len);
            strncpy(path, slash, sizeof(path) - 1);
        } else {
            strncpy(service, service_arg, sizeof(service) - 1);
        }
    }

    for (sni_item_t *it = sni_items; it; it = it->next) {
        if (strcmp(it->service, service) == 0 && strcmp(it->path, path) == 0) return;
    }

    sni_item_t *item = calloc(1, sizeof(sni_item_t));
    if (!item) return;

    item->service = strdup(service);
    item->path = strdup(path);
    fetch_sni_icon(item);

    item->next = sni_items;
    sni_items = item;
}

static DBusHandlerResult handle_dbus_message(DBusConnection *conn, DBusMessage *msg, void *data) {
    (void)data;
    if (dbus_message_is_method_call(msg, "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem")) {
        const char *service_arg = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &service_arg, DBUS_TYPE_INVALID)) {
            add_sni_item(service_arg);

            for (pbar_output_t *out = outputs; out; out = out->next) {
                draw_output(out);
                wayland_update(out);
            }
        }

        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void init_dbus_sni(void) {
    DBusError err;
    dbus_error_init(&err);

    dbus_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!dbus_conn) {
        dbus_error_free(&err);
        return;
    }

    dbus_bus_request_name(dbus_conn, "org.kde.StatusNotifierWatcher", DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return;
    }

    dbus_bus_add_match(dbus_conn, "type='method_call',interface='org.kde.StatusNotifierWatcher'", &err);
    dbus_connection_add_filter(dbus_conn, handle_dbus_message, NULL, NULL);
}

/* --- Systray Backend Contract Implementation --- */

static uint32_t backend_systray_get_width(void) {
    int count = 0;
    for (sni_item_t *it = sni_items; it; it = it->next) count++;

    if (count == 0) return 0;
    return count * TRAY_ICON_SIZE + (count - 1) * TRAY_ICON_SPACING + (TRAY_PADDING_X * 2);
}

static void backend_systray_draw(pbar_output_t *out, int x_offset, uint32_t width) {
    int cur_x = x_offset + TRAY_PADDING_X;
    int cur_y = (out->height - TRAY_ICON_SIZE) / 2;
    if (cur_y < 0) cur_y = 0;

    for (sni_item_t *item = sni_items; item; item = item->next) {
        if (!item->pixels || item->width <= 0 || item->height <= 0) continue;

        for (int dy = 0; dy < TRAY_ICON_SIZE; dy++) {
            int py = cur_y + dy;
            if (py < 0 || py >= out->height) continue;

            int sy = (dy * item->height) / TRAY_ICON_SIZE;

            for (int dx = 0; dx < TRAY_ICON_SIZE; dx++) {
                int px = cur_x + dx;
                if (px < x_offset || px >= out->width || px >= (int)(x_offset + width)) continue;

                int sx = (dx * item->width) / TRAY_ICON_SIZE;
                uint32_t fg_pixel = item->pixels[sy * item->width + sx];

                uint8_t alpha = (fg_pixel >> 24) & 0xFF;
                if (alpha == 0) continue;

                uint32_t *dst = &out->pixels[py * out->width + px];
                *dst = blend_pixel(*dst, fg_pixel, alpha);
            }
        }
        cur_x += TRAY_ICON_SIZE + TRAY_ICON_SPACING;
    }
}

/* --- Wayland Buffer Helpers --- */

static void wayland_create_buffer(pbar_output_t *out) {
    wayland_state_t *st = out->b_state;
    if (st->wl_buffer) {
        wl_buffer_destroy(st->wl_buffer);
        st->wl_buffer = NULL;
    }
    if (st->shm_pool) {
        wl_shm_pool_destroy(st->shm_pool);
        st->shm_pool = NULL;
    }

    int stride = out->width * 4;
    st->shm_pool = wl_shm_create_pool(wl_shm_inst, out->shm_fd, out->shm_size);
    st->wl_buffer = wl_shm_pool_create_buffer(st->shm_pool, 0, out->width, out->height,
                                              stride, WL_SHM_FORMAT_ARGB8888);
}

static void wayland_update(pbar_output_t *out) {
    wayland_state_t *st = out->b_state;
    if (!st->configured || !st->wl_surface) return;

    if (!st->wl_buffer) {
        wayland_create_buffer(out);
    }

    wl_surface_attach(st->wl_surface, st->wl_buffer, 0, 0);
    wl_surface_damage_buffer(st->wl_surface, 0, 0, out->width, out->height);
    wl_surface_commit(st->wl_surface);
}

/* --- Layer Surface & Registry Listeners --- */

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                           uint32_t serial, uint32_t width, uint32_t height) {
    pbar_output_t *out = data;
    wayland_state_t *st = out->b_state;
    
    if (width > 0) out->width = width;
    if (height > 0) out->height = height;

    zwlr_layer_surface_v1_ack_configure(surface, serial);
    st->configured = true;

    draw_output(out);
    wayland_update(out);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)data;
    (void)surface;
    running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                    uint32_t name, const char *interface, uint32_t version) {
    (void)data; (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wl_comp = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wl_shm_inst = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (TARGET_MONITOR == -1 || TARGET_MONITOR == output_count) {
            pbar_output_t *out_node = calloc(1, sizeof(pbar_output_t));
            if (!out_node) return;

            out_node->b_state = calloc(1, sizeof(wayland_state_t));
            if (!out_node->b_state) { free(out_node); return; }

            out_node->id = output_count;
            out_node->width = 1920;
            out_node->height = BAR_HEIGHT;

            out_node->next = outputs;
            outputs = out_node;
        }
        output_count++;
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* --- Backend Entry Point --- */

static int backend_run(void) {
    int r = -1;

    wl_disp = wl_display_connect(NULL);
    if (!wl_disp) {
        die("could not connect to wayland display");
        goto out;
    }

    wl_reg = wl_display_get_registry(wl_disp);
    wl_registry_add_listener(wl_reg, &registry_listener, NULL);
    wl_display_roundtrip(wl_disp);

    if (!wl_comp || !wl_shm_inst || !layer_shell) {
        die("missing required wayland globals (compositor, shm, or layer-shell)");
        goto out_disp;
    }

    init_dbus_sni();

    for (pbar_output_t *out_node = outputs; out_node; out_node = out_node->next) {
        wayland_state_t *st = out_node->b_state;

        if (init_shm(out_node) < 0) {
            die("failed to initialize SHM buffer for wayland output");
            goto out_disp;
        }

        st->wl_surface = wl_compositor_create_surface(wl_comp);
        st->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell, st->wl_surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "pbar"
        );

        zwlr_layer_surface_v1_set_size(st->layer_surface, 0, BAR_HEIGHT);
        zwlr_layer_surface_v1_set_anchor(
            st->layer_surface,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
        );
        zwlr_layer_surface_v1_set_exclusive_zone(st->layer_surface, BAR_HEIGHT);
        zwlr_layer_surface_v1_add_listener(st->layer_surface, &layer_surface_listener, out_node);

        wl_surface_commit(st->wl_surface);
    }

    wl_display_roundtrip(wl_disp);

    int wl_fd = wl_display_get_fd(wl_disp);
    int dbus_fd = -1;
    if (dbus_conn) dbus_connection_get_unix_fd(dbus_conn, &dbus_fd);

    struct pollfd pfds[2] = {
        { .fd = wl_fd,   .events = POLLIN },
        { .fd = dbus_fd, .events = POLLIN }
    };

    while (running) {
        wl_display_dispatch_pending(wl_disp);
        wl_display_flush(wl_disp);

        int nfds = (dbus_fd >= 0) ? 2 : 1;
        if (poll(pfds, nfds, -1) < 0) {
            if (errno == EINTR && !running) break;
            continue;
        }

        if (pfds[0].revents & POLLIN) {
            if (wl_display_dispatch(wl_disp) < 0) break;
        }

        if (dbus_fd >= 0 && (pfds[1].revents & POLLIN)) {
            dbus_connection_read_write_dispatch(dbus_conn, 0);
        }
    }

    r = 0;

out_disp:
    for (sni_item_t *it = sni_items; it; ) {
        sni_item_t *next = it->next;
        free(it->service);
        free(it->path);
        free(it->pixels);
        free(it);
        it = next;
    }
    sni_items = NULL;

    if (dbus_conn) dbus_connection_unref(dbus_conn);

    for (pbar_output_t *out_node = outputs; out_node; ) {
        pbar_output_t *next = out_node->next;
        wayland_state_t *st = out_node->b_state;
        if (st) {
            if (st->wl_buffer) wl_buffer_destroy(st->wl_buffer);
            if (st->shm_pool) wl_shm_pool_destroy(st->shm_pool);
            if (st->layer_surface) zwlr_layer_surface_v1_destroy(st->layer_surface);
            if (st->wl_surface) wl_surface_destroy(st->wl_surface);
            free(st);
        }
        if (out_node->pixels) munmap(out_node->pixels, out_node->shm_size);
        if (out_node->shm_fd >= 0) close(out_node->shm_fd);
        free(out_node);
        out_node = next;
    }
    outputs = NULL;

    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (wl_shm_inst) wl_shm_destroy(wl_shm_inst);
    if (wl_comp) wl_compositor_destroy(wl_comp);
    if (wl_reg) wl_registry_destroy(wl_reg);
    wl_display_disconnect(wl_disp);

out:
    return r;
}

#endif /* BACKEND_WAYLAND_H */
