#ifndef BACKEND_X11_H
#define BACKEND_X11_H

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/randr.h>

typedef struct {
    xcb_window_t x_win;
    xcb_shm_seg_t x_shm_seg;
} x11_state_t;

static xcb_connection_t *x_conn;
static xcb_screen_t *x_screen;
static xcb_gcontext_t x_gc;

static void x11_update(pbar_output_t *out) {
    x11_state_t *st = out->b_state;
    xcb_shm_put_image(x_conn, st->x_win, x_gc, out->width, out->height,
                      0, 0, out->width, out->height, 0, 0, 24,
                      XCB_IMAGE_FORMAT_Z_PIXMAP, 0, st->x_shm_seg, 0);
    xcb_flush(x_conn);
}

static int backend_run(void) {
    int r = -1;

    xcb_intern_atom_reply_t *r_type = NULL;
    xcb_intern_atom_reply_t *r_dock = NULL;
    xcb_intern_atom_reply_t *r_state = NULL;
    xcb_intern_atom_reply_t *r_sticky = NULL;
    xcb_intern_atom_reply_t *r_above = NULL;
    xcb_intern_atom_reply_t *r_strut = NULL;
    xcb_intern_atom_reply_t *r_strut_p = NULL;

    x_conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(x_conn)) {
        die("could not connect to X11");
        goto out;
    }

    x_screen = xcb_setup_roots_iterator(xcb_get_setup(x_conn)).data;
    if (!x_screen) {
        die("could not get X11 screen");
        goto out_conn;
    }

    x_gc = xcb_generate_id(x_conn);
    xcb_create_gc(x_conn, x_gc, x_screen->root, 0, NULL);

    /* Monitor Query via RandR */
    xcb_randr_get_screen_resources_current_cookie_t res_c =
        xcb_randr_get_screen_resources_current(x_conn, x_screen->root);
    xcb_randr_get_screen_resources_current_reply_t *res_r =
        xcb_randr_get_screen_resources_current_reply(x_conn, res_c, NULL);

    if (!res_r) {
        die("failed to get xrandr resources");
        goto out_gc;
    }

    xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(res_r);
    int crtc_len = xcb_randr_get_screen_resources_current_crtcs_length(res_r);

    for (int i = 0; i < crtc_len; i++) {
        xcb_randr_get_crtc_info_reply_t *crtc = xcb_randr_get_crtc_info_reply(
            x_conn, xcb_randr_get_crtc_info(x_conn, crtcs[i], res_r->config_timestamp), NULL);

        if (crtc && crtc->width > 0) {
            if (TARGET_MONITOR == -1 || TARGET_MONITOR == output_count) {
                pbar_output_t *out_node = calloc(1, sizeof(pbar_output_t));
                if (!out_node) { free(crtc); goto out_res; }
                out_node->b_state = calloc(1, sizeof(x11_state_t));
                if (!out_node->b_state) { free(out_node); free(crtc); goto out_res; }

                out_node->id = output_count;
                out_node->x = crtc->x;
                out_node->y = crtc->y;
                out_node->width = crtc->width;
                out_node->height = BAR_HEIGHT;

                out_node->next = outputs;
                outputs = out_node;
            }
            output_count++;
        }
        free(crtc);
    }

    #define GET_ATOM(name) xcb_intern_atom_reply(x_conn, xcb_intern_atom(x_conn, 0, strlen(name), name), NULL)
    r_type    = GET_ATOM("_NET_WM_WINDOW_TYPE");
    r_dock    = GET_ATOM("_NET_WM_WINDOW_TYPE_DOCK");
    r_state   = GET_ATOM("_NET_WM_STATE");
    r_sticky  = GET_ATOM("_NET_WM_STATE_STICKY");
    r_above   = GET_ATOM("_NET_WM_STATE_ABOVE");
    r_strut   = GET_ATOM("_NET_WM_STRUT");
    r_strut_p = GET_ATOM("_NET_WM_STRUT_PARTIAL");
    #undef GET_ATOM

    /* Setup Windows */
    for (pbar_output_t *out_node = outputs; out_node; out_node = out_node->next) {
        x11_state_t *st = out_node->b_state;
        st->x_win = xcb_generate_id(x_conn);

        uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        uint32_t values[2] = { x_screen->black_pixel, XCB_EVENT_MASK_EXPOSURE };

        xcb_create_window(x_conn, XCB_COPY_FROM_PARENT, st->x_win, x_screen->root,
                          out_node->x, out_node->y, out_node->width, out_node->height, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, x_screen->root_visual, mask, values);

        if (r_type && r_dock) {
            xcb_change_property(x_conn, XCB_PROP_MODE_REPLACE, st->x_win,
                                r_type->atom, XCB_ATOM_ATOM, 32, 1, &r_dock->atom);
        }

        if (r_state && r_sticky && r_above) {
            xcb_atom_t states[2] = { r_sticky->atom, r_above->atom };
            xcb_change_property(x_conn, XCB_PROP_MODE_REPLACE, st->x_win,
                                r_state->atom, XCB_ATOM_ATOM, 32, 2, states);
        }

        uint32_t top_reserve = out_node->y + out_node->height;
        if (r_strut) {
            uint32_t strut[4] = { 0, 0, top_reserve, 0 };
            xcb_change_property(x_conn, XCB_PROP_MODE_REPLACE, st->x_win,
                                r_strut->atom, XCB_ATOM_CARDINAL, 32, 4, strut);
        }

        if (r_strut_p) {
            uint32_t strut_p[12] = { 0 };
            strut_p[2] = top_reserve;
            strut_p[8] = out_node->x;
            strut_p[9] = out_node->x + out_node->width - 1;
            xcb_change_property(x_conn, XCB_PROP_MODE_REPLACE, st->x_win,
                                r_strut_p->atom, XCB_ATOM_CARDINAL, 32, 12, strut_p);
        }

        if (init_shm(out_node) < 0) {
            die("failed to initialize SHM buffer");
            goto out_res;
        }

        st->x_shm_seg = xcb_generate_id(x_conn);
        xcb_shm_attach(x_conn, st->x_shm_seg, out_node->shm_fd, 0);
        xcb_map_window(x_conn, st->x_win);

        draw_output(out_node);
        x11_update(out_node);
    }
    xcb_flush(x_conn);

    /* Main Event Loop */
    struct pollfd pfd = {
        .fd = xcb_get_file_descriptor(x_conn),
        .events = POLLIN
    };

    while (running) {
        if (poll(&pfd, 1, -1) < 0) continue;

        if (pfd.revents & POLLIN) {
            xcb_generic_event_t *ev;
            while ((ev = xcb_poll_for_event(x_conn))) {
                if ((ev->response_type & ~0x80) == XCB_EXPOSE) {
                    xcb_expose_event_t *expose = (xcb_expose_event_t *)ev;
                    for (pbar_output_t *out_node = outputs; out_node; out_node = out_node->next) {
                        x11_state_t *st = out_node->b_state;
                        if (st->x_win == expose->window) {
                            x11_update(out_node);
                            break;
                        }
                    }
                }
                free(ev);
            }
        }
    }

    r = 0;

out_res:
    for (pbar_output_t *out_node = outputs; out_node; ) {
        pbar_output_t *next = out_node->next;
        x11_state_t *st = out_node->b_state;
        if (st) {
            if (r_strut && r_strut->atom) xcb_delete_property(x_conn, st->x_win, r_strut->atom);
            if (r_strut_p && r_strut_p->atom) xcb_delete_property(x_conn, st->x_win, r_strut_p->atom);
            xcb_unmap_window(x_conn, st->x_win);
            if (st->x_shm_seg) xcb_shm_detach(x_conn, st->x_shm_seg);
            xcb_destroy_window(x_conn, st->x_win);
            free(st);
        }
        if (out_node->pixels) munmap(out_node->pixels, out_node->shm_size);
        if (out_node->shm_fd >= 0) close(out_node->shm_fd);
        free(out_node);
        out_node = next;
    }
    outputs = NULL;

    free(res_r);
    free(r_type); free(r_dock); free(r_state);
    free(r_sticky); free(r_above); free(r_strut); free(r_strut_p);

out_gc:
    xcb_free_gc(x_conn, x_gc);

out_conn:
    xcb_flush(x_conn);
    xcb_disconnect(x_conn);

out:
    return r;
}

#endif /* BACKEND_X11_H */
