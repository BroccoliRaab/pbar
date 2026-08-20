#ifndef BACKEND_X11_H
#define BACKEND_X11_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

#include "pbar.h"
#include "config.h"

/* --- System Tray State --- */
static xcb_connection_t *tray_c = NULL;
static xcb_window_t tray_win = 0;
static xcb_atom_t opcode_atom = XCB_NONE;

typedef struct tray_icon {
    xcb_window_t win;
    struct tray_icon *next;
} tray_icon_t;

static tray_icon_t *tray_icons = NULL;
static int tray_icon_count = 0;

/* --- XCB Atom Helper --- */
static inline xcb_atom_t backend_get_atom(xcb_connection_t *c, const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(c, 0, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(c, cookie, NULL);
    if (!reply) return XCB_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

/* --- System Tray Manager Init & Events --- */
static inline void x11_systray_init(xcb_connection_t *c, xcb_screen_t *screen, xcb_window_t win) {
    tray_c = c;
    tray_win = win;
    
    opcode_atom = backend_get_atom(c, "_NET_SYSTEM_TRAY_OPCODE");
    xcb_atom_t tray_atom = backend_get_atom(c, "_NET_SYSTEM_TRAY_S0");
    xcb_atom_t manager_atom = backend_get_atom(c, "MANAGER");
    
    xcb_set_selection_owner(c, win, tray_atom, XCB_CURRENT_TIME);
    
    xcb_client_message_event_t msg = {0};
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = screen->root;
    msg.type = manager_atom;
    msg.data.data32[0] = XCB_CURRENT_TIME;
    msg.data.data32[1] = tray_atom;
    msg.data.data32[2] = win;
    
    xcb_send_event(c, 0, screen->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&msg);
    xcb_flush(c);
}

static inline void x11_systray_handle_event(xcb_generic_event_t *ev) {
    if (!tray_c) return;
    
    uint8_t type = ev->response_type & ~0x80;
    
    if (type == XCB_CLIENT_MESSAGE) {
        xcb_client_message_event_t *cme = (xcb_client_message_event_t *)ev;
        if (cme->type == opcode_atom && cme->data.data32[1] == 0) {
            xcb_window_t icon_win = cme->data.data32[2];
            
            xcb_reparent_window(tray_c, icon_win, tray_win, 0, 0);
            
            tray_icon_t *ti = malloc(sizeof(tray_icon_t));
            ti->win = icon_win;
            ti->next = tray_icons;
            tray_icons = ti;
            tray_icon_count++;
            
            xcb_map_window(tray_c, icon_win);
            
            uint32_t values[] = { XCB_EVENT_MASK_STRUCTURE_NOTIFY };
            xcb_change_window_attributes(tray_c, icon_win, XCB_CW_EVENT_MASK, values);
        }
    } else if (type == XCB_DESTROY_NOTIFY || type == XCB_UNMAP_NOTIFY) {
        xcb_destroy_notify_event_t *dne = (xcb_destroy_notify_event_t *)ev;
        tray_icon_t **curr = &tray_icons;
        while (*curr) {
            if ((*curr)->win == dne->window) {
                tray_icon_t *tmp = *curr;
                *curr = (*curr)->next;
                free(tmp);
                tray_icon_count--;
                break;
            }
            curr = &(*curr)->next;
        }
    }
}

/* --- Backend API implemented for pbar.c --- */

uint32_t backend_systray_get_width(pbar_output_t *out) {
    /* If this specific monitor is NOT the tray monitor, return 0 width! */
    if (!tray_c || out->b_state != (void*)(uintptr_t)tray_win) return 0;
    return (tray_icon_count > 0) ? (tray_icon_count * TRAY_ICON_SIZE + (tray_icon_count - 1) * TRAY_ICON_SPACING) : 0;
}

void backend_systray_draw(pbar_output_t *out, int x) {
    if (!tray_c || out->b_state != (void*)(uintptr_t)tray_win) return;
    
    int current_x = x;
    for (tray_icon_t *icon = tray_icons; icon; icon = icon->next) {
        uint32_t values[] = { current_x, (BAR_HEIGHT - TRAY_ICON_SIZE) / 2, TRAY_ICON_SIZE, TRAY_ICON_SIZE };
        xcb_configure_window(tray_c, icon->win, 
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | 
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
        current_x += TRAY_ICON_SIZE + TRAY_ICON_SPACING;
    }
}

/* --- Window Manager Struts --- */
static inline void set_window_strut(xcb_connection_t *c, xcb_window_t win, int x, int width) {
    xcb_atom_t net_wm_strut_partial = backend_get_atom(c, "_NET_WM_STRUT_PARTIAL");
    xcb_atom_t net_wm_strut = backend_get_atom(c, "_NET_WM_STRUT");
    xcb_atom_t net_wm_window_type = backend_get_atom(c, "_NET_WM_WINDOW_TYPE");
    xcb_atom_t net_wm_window_type_dock = backend_get_atom(c, "_NET_WM_WINDOW_TYPE_DOCK");

    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, net_wm_window_type, XCB_ATOM_ATOM, 32, 1, &net_wm_window_type_dock);

    uint32_t strut[12] = {0};
    strut[2] = BAR_HEIGHT;          
    strut[8] = x;                   
    strut[9] = x + width - 1;       

    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, net_wm_strut_partial, XCB_ATOM_CARDINAL, 32, 12, strut);
    
    uint32_t strut_simple[4] = {0, 0, BAR_HEIGHT, 0};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, net_wm_strut, XCB_ATOM_CARDINAL, 32, 4, strut_simple);
}

/* --- Main XCB Loop --- */
int backend_run(void) {
    int screen_nbr;
    xcb_connection_t *c = xcb_connect(NULL, &screen_nbr);
    if (xcb_connection_has_error(c)) die("Cannot open XCB connection");

    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen_nbr; ++i) xcb_screen_next(&iter);
    xcb_screen_t *screen = iter.data;

    xcb_gcontext_t gc = xcb_generate_id(c);
    uint32_t gc_mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
    uint32_t gc_values[] = { screen->black_pixel, screen->white_pixel };
    xcb_create_gc(c, gc, screen->root, gc_mask, gc_values);

    xcb_randr_query_version_cookie_t v_cookie = xcb_randr_query_version(c, 1, 1);
    xcb_randr_query_version_reply_t *v_reply = xcb_randr_query_version_reply(c, v_cookie, NULL);
    if (v_reply) free(v_reply);

    xcb_randr_get_screen_resources_current_cookie_t res_cookie = xcb_randr_get_screen_resources_current(c, screen->root);
    xcb_randr_get_screen_resources_current_reply_t *res = xcb_randr_get_screen_resources_current_reply(c, res_cookie, NULL);
    if (!res) die("Failed to get XRandR screen resources");

    int id_counter = 0;
    int output_len = xcb_randr_get_screen_resources_current_outputs_length(res);
    xcb_randr_output_t *r_outputs = xcb_randr_get_screen_resources_current_outputs(res);

    xcb_window_t target_tray_win = 0; /* Keep track of the chosen systray parent */

    for (int i = 0; i < output_len; i++) {
        xcb_randr_get_output_info_cookie_t out_cookie = xcb_randr_get_output_info(c, r_outputs[i], res->config_timestamp);
        xcb_randr_get_output_info_reply_t *out_info = xcb_randr_get_output_info_reply(c, out_cookie, NULL);

        if (out_info && out_info->crtc != XCB_NONE && out_info->connection == XCB_RANDR_CONNECTION_CONNECTED) {
            xcb_randr_get_crtc_info_cookie_t crtc_cookie = xcb_randr_get_crtc_info(c, out_info->crtc, res->config_timestamp);
            xcb_randr_get_crtc_info_reply_t *crtc = xcb_randr_get_crtc_info_reply(c, crtc_cookie, NULL);

            if (crtc) {
                int name_len = xcb_randr_get_output_info_name_length(out_info);
                uint8_t *name_ptr = xcb_randr_get_output_info_name(out_info);
                char name[256];
                snprintf(name, sizeof(name), "%.*s", name_len, name_ptr);

                int should_draw = 0;
                if (target_monitors[0] == NULL) {
                    should_draw = 1;
                } else {
                    for (int m = 0; target_monitors[m] != NULL; m++) {
                        if (strcmp(name, target_monitors[m]) == 0) {
                            should_draw = 1;
                            break;
                        }
                    }
                }

                if (should_draw) {
                    pbar_output_t *out = calloc(1, sizeof(pbar_output_t));
                    out->id = ++id_counter;
                    out->x = crtc->x;
                    out->y = crtc->y;
                    out->width = crtc->width;
                    out->height = BAR_HEIGHT;
                    
                    if (init_shm(out) < 0) die("Failed to initialize SHM buffer");

                    xcb_window_t win = xcb_generate_id(c);
                    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
                    uint32_t values[] = {
                        COLOR_BG, 1, 
                        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY
                    };

                    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root,
                        out->x, out->y, out->width, out->height, 0,
                        XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                        mask, values);

                    set_window_strut(c, win, out->x, out->width);
                    xcb_map_window(c, win);
                    out->b_state = (void *)(uintptr_t)win;

                    /* Assign target tray monitor if it matches config */
                    #ifdef SYSTRAY_MONITOR
                    if (strcmp(name, SYSTRAY_MONITOR) == 0) {
                        target_tray_win = win;
                    }
                    #endif
                    
                    /* Fallback: If not specified, use the first generated monitor */
                    if (!target_tray_win) {
                        target_tray_win = win;
                    }

                    out->next = outputs;
                    outputs = out;
                    output_count++;
                }
                free(crtc);
            }
        }
        if (out_info) free(out_info);
    }
    free(res);

    if (output_count == 0) die("No matching monitors found.");

    /* Initialize systray strictly on the designated window */
    if (target_tray_win) {
        x11_systray_init(c, screen, target_tray_win);
    }

    int xcb_fd = xcb_get_file_descriptor(c);

    while (running) {
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(c))) {
            x11_systray_handle_event(ev); 
            free(ev);
        }

        for (pbar_output_t *out = outputs; out != NULL; out = out->next) {
            draw_output(out);
            
            xcb_window_t win = (xcb_window_t)(uintptr_t)out->b_state;
            xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, win, gc,
                          out->width, out->height, 0, 0, 0, screen->root_depth,
                          out->width * out->height * 4, (uint8_t *)out->pixels);
        }
        
        xcb_flush(c);

        fd_set in_fds;
        FD_ZERO(&in_fds);
        FD_SET(xcb_fd, &in_fds);

        struct timeval tv;
        tv.tv_sec = 1; tv.tv_usec = 0;
        select(xcb_fd + 1, &in_fds, NULL, NULL, &tv);
    }

    xcb_disconnect(c);
    return 0;
}

#endif /* BACKEND_X11_H */
