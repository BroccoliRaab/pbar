#ifndef BACKEND_X11_H
#define BACKEND_X11_H

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

/* XEmbed System Tray Opcodes */
#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_EMBEDDED_NOTIFY   0

/* --- Backend Private State --- */

typedef struct {
    xcb_window_t window;
    xcb_gcontext_t gc;
    xcb_shm_seg_t shm_seg[2];
    bool mapped;
} x11_state_t;

typedef struct tray_client_s {
    xcb_window_t win;
    struct tray_client_s *next;
} tray_client_t;

static xcb_connection_t *conn = NULL;
static xcb_screen_t *screen = NULL;

static xcb_atom_t atom_net_system_tray = XCB_NONE;
static xcb_atom_t atom_net_system_tray_opcode = XCB_NONE;
static xcb_atom_t atom_xembed = XCB_NONE;
static xcb_atom_t atom_xembed_info = XCB_NONE;

static tray_client_t *tray_clients = NULL;

static xcb_atom_t get_atom(const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
    if (!reply) return XCB_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

static void x11_update(pbar_output_t *out) {
    x11_state_t *st = out->b_state;
    if (!st || !st->window || !out->pixels) return;

    xcb_shm_put_image(
        conn,
        st->window,
        st->gc,
        out->width, out->height,
        0, 0,
        out->width, out->height,
        0, 0,
        24,
        XCB_IMAGE_FORMAT_Z_PIXMAP,
        0,
        st->shm_seg[out->current_buf],
        0
    );
    xcb_flush(conn);
}

/* --- X11 XEmbed System Tray Logic --- */

static void add_tray_client(xcb_window_t client_win) {
    for (tray_client_t *c = tray_clients; c; c = c->next) {
        if (c->win == client_win) return;
    }

    tray_client_t *client = calloc(1, sizeof(tray_client_t));
    if (!client) return;

    client->win = client_win;

    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t values[] = { XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(conn, client_win, mask, values);

    client->next = tray_clients;
    tray_clients = client;
}

static void remove_tray_client(xcb_window_t client_win) {
    tray_client_t **curr = &tray_clients;
    while (*curr) {
        if ((*curr)->win == client_win) {
            tray_client_t *tmp = *curr;
            *curr = (*curr)->next;
            free(tmp);
            return;
        }
        curr = &(*curr)->next;
    }
}

static void init_x11_systray(pbar_output_t *out) {
    x11_state_t *st = out->b_state;
    if (!st) return;

    char sel_name[64];
    snprintf(sel_name, sizeof(sel_name), "_NET_SYSTEM_TRAY_S%d", 0);

    atom_net_system_tray = get_atom(sel_name);
    atom_net_system_tray_opcode = get_atom("_NET_SYSTEM_TRAY_OPCODE");
    atom_xembed = get_atom("_XEMBED");
    atom_xembed_info = get_atom("_XEMBED_INFO");

    xcb_set_selection_owner(conn, st->window, atom_net_system_tray, XCB_TIME_CURRENT_TIME);

    xcb_get_selection_owner_cookie_t cookie = xcb_get_selection_owner(conn, atom_net_system_tray);
    xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(conn, cookie, NULL);

    if (reply && reply->owner == st->window) {
        xcb_client_message_event_t ev = {
            .response_type = XCB_CLIENT_MESSAGE,
            .format = 32,
            .window = screen->root,
            .type = get_atom("MANAGER"),
            .data.data32 = {
                XCB_TIME_CURRENT_TIME,
                atom_net_system_tray,
                st->window,
                0, 0
            }
        };

        xcb_send_event(conn, 0, screen->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
    }
    free(reply);
}

/* --- Systray Backend Contract Implementation --- */

static uint32_t backend_systray_get_width(void) {
    int count = 0;
    for (tray_client_t *c = tray_clients; c; c = c->next) count++;

    if (count == 0) return 0;
    return count * TRAY_ICON_SIZE + (count - 1) * TRAY_ICON_SPACING + (TRAY_PADDING_X * 2);
}

static void backend_systray_draw(pbar_output_t *out, int x_offset, uint32_t width) {
    (void)width;
    x11_state_t *st = out->b_state;
    if (!st || !st->window) return;

    int cur_x = x_offset + TRAY_PADDING_X;
    int cur_y = (out->height - TRAY_ICON_SIZE) / 2;
    if (cur_y < 0) cur_y = 0;

    for (tray_client_t *c = tray_clients; c; c = c->next) {
        xcb_reparent_window(conn, c->win, st->window, cur_x, cur_y);

        uint32_t values[] = { cur_x, cur_y, TRAY_ICON_SIZE, TRAY_ICON_SIZE };
        xcb_configure_window(conn, c->win, 
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, 
            values);

        xcb_map_window(conn, c->win);

        xcb_client_message_event_t xev = {
            .response_type = XCB_CLIENT_MESSAGE,
            .format = 32,
            .window = c->win,
            .type = atom_xembed,
            .data.data32 = { XCB_TIME_CURRENT_TIME, XEMBED_EMBEDDED_NOTIFY, 0, st->window, 0 }
        };
        xcb_send_event(conn, 0, c->win, XCB_EVENT_MASK_NO_EVENT, (const char *)&xev);

        cur_x += TRAY_ICON_SIZE + TRAY_ICON_SPACING;
    }
    xcb_flush(conn);
}

/* --- Backend Entry Point --- */

static int backend_run(void) {
    int r = -1;

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        die("could not connect to X11 display");
        goto out;
    }

    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    if (!screen) {
        die("failed to retrieve X11 screen");
        goto out_conn;
    }

    pbar_output_t *out_node = calloc(1, sizeof(pbar_output_t));
    if (!out_node) goto out_conn;

    out_node->b_state = calloc(1, sizeof(x11_state_t));
    if (!out_node->b_state) { free(out_node); goto out_conn; }

    out_node->id = 0;
    out_node->width = screen->width_in_pixels;
    out_node->height = BAR_HEIGHT;
    out_node->next = outputs;
    outputs = out_node;
    output_count = 1;

    x11_state_t *st = out_node->b_state;

    if (init_shm(out_node) < 0) {
        die("failed to initialize SHM buffers for X11 output");
        goto out_conn;
    }

    /* Attach BOTH memfd segments to the X server */
    for (int i = 0; i < 2; i++) {
        st->shm_seg[i] = xcb_generate_id(conn);
        xcb_shm_attach_fd(conn, st->shm_seg[i], out_node->shm_fd[i], 0);
    }

    /* Create X11 Bar Window */
    st->window = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        COLOR_BG,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
    };

    xcb_create_window(
        conn,
        XCB_COPY_FROM_PARENT,
        st->window,
        screen->root,
        0, 0,
        out_node->width, out_node->height,
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,
        mask, values
    );

    st->gc = xcb_generate_id(conn);
    xcb_create_gc(conn, st->gc, st->window, 0, NULL);

    /* Set Window Manager Dock Properties */
    xcb_atom_t atom_type = get_atom("_NET_WM_WINDOW_TYPE");
    xcb_atom_t atom_dock = get_atom("_NET_WM_WINDOW_TYPE_DOCK");
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, st->window, atom_type, XCB_ATOM_ATOM, 32, 1, &atom_dock);

    /* Reserve desktop space at top */
    xcb_atom_t atom_strut = get_atom("_NET_WM_STRUT_PARTIAL");
    uint32_t strut[12] = { 0, 0, BAR_HEIGHT, 0, 0, 0, 0, 0, 0, out_node->width, 0, 0 };
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, st->window, atom_strut, XCB_ATOM_CARDINAL, 32, 12, strut);

    init_x11_systray(out_node);

    xcb_map_window(conn, st->window);
    xcb_flush(conn);

    int x11_fd = xcb_get_file_descriptor(conn);
    struct pollfd pfd = { .fd = x11_fd, .events = POLLIN };

    while (running) {
        if (poll(&pfd, 1, 1000) < 0) {
            if (errno == EINTR && !running) break;
            continue;
        }

        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(conn))) {
            uint8_t response = ev->response_type & ~0x80;

            switch (response) {
            case XCB_CLIENT_MESSAGE: {
                xcb_client_message_event_t *cme = (xcb_client_message_event_t *)ev;
                if (cme->type == atom_net_system_tray_opcode && cme->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK) {
                    add_tray_client(cme->data.data32[2]);
                }
                break;
            }

            case XCB_DESTROY_NOTIFY: {
                xcb_destroy_notify_event_t *dne = (xcb_destroy_notify_event_t *)ev;
                remove_tray_client(dne->window);
                break;
            }

            case XCB_UNMAP_NOTIFY: {
                xcb_unmap_notify_event_t *une = (xcb_unmap_notify_event_t *)ev;
                remove_tray_client(une->window);
                break;
            }

            default:
                break;
            }

            free(ev);
        }

        /* Draw handles the pointer swap internally, then x11_update reads the new buffer */
        draw_output(out_node);
        x11_update(out_node);
    }

    r = 0;

out_conn:
    for (tray_client_t *c = tray_clients; c; ) {
        tray_client_t *next = c->next;
        free(c);
        c = next;
    }
    tray_clients = NULL;

    for (pbar_output_t *out = outputs; out; ) {
        pbar_output_t *next = out->next;
        x11_state_t *state = out->b_state;
        
        if (state) {
            if (state->gc) xcb_free_gc(conn, state->gc);
            if (state->window) xcb_destroy_window(conn, state->window);
            
            /* Detach BOTH memfd segments */
            for (int i = 0; i < 2; i++) {
                if (state->shm_seg[i]) {
                    xcb_shm_detach(conn, state->shm_seg[i]);
                }
            }
            free(state);
        }
        
        /* Unmap and close BOTH buffers cleanly */
        for (int i = 0; i < 2; i++) {
            if (out->shm_pixels[i]) munmap(out->shm_pixels[i], out->shm_size);
            if (out->shm_fd[i] >= 0) close(out->shm_fd[i]);
        }
        
        free(out);
        out = next;
    }
    outputs = NULL;

    if (conn) xcb_disconnect(conn);

out:
    return r;
}

#endif /* BACKEND_X11_H */
