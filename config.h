#ifndef CONFIG_H
#define CONFIG_H

#include "pbar.h"

/* --- General Options --- */
#define FONT_PATH "/usr/share/fonts/TTF/DejaVuSans.ttf"
#define FONT_SIZE 14.0f

#define COLOR_BG 0xFF222222
#define COLOR_FG 0xFFEEEEEE

#define BAR_HEIGHT 24
#define TRAY_ICON_SIZE 16
#define TRAY_ICON_SPACING 4
#define TRAY_PADDING_X 4

/* --- Monitor Selection --- */
/* Add the X11 names of the monitors you want to draw to (e.g., "eDP-1", "DP-1").
 * If the first item is NULL, the bar will draw on ALL connected monitors. */
static const char *target_monitors[] = {
    NULL
};

/* --- Left Aligned Elements --- */
pbar_element_t *left_elements[] = {
    (pbar_element_t *) &(exec_element_t) {
        .text_base = {
            .base = { .think = exec_think, .draw = exec_draw },
            .color = 0xFF88AAFF,
            .padding_x = 10
        },
        .cmd = "echo $PBAR_MONITOR_NAME",
        .interval_sec = 60
    },
    (pbar_element_t *) &(exec_element_t) {
        .text_base = {
            .base = { .think = exec_think, .draw = exec_draw },
            .color = 0xFF88AAFF,
            .padding_x = 10
        },
        .cmd = "bspc query -D -m $PBAR_MONITOR_NAME --names -d .active",
        .interval_sec = 1
    },
    NULL
};

/* --- Center Aligned Elements --- */
pbar_element_t *middle_elements[] = {
    (pbar_element_t *) &(clock_element_t) {
        .text_base = {
            .base = { .think = clock_think, .draw = clock_draw },
            .text = "",
            .color = COLOR_FG,
            .padding_x = 10
        },
        .format = "%A %b %d %r"
    },
    NULL
};

/* --- Right Aligned Elements --- */
pbar_element_t *right_elements[] = {
    (pbar_element_t *) &(systray_element_t) {
        .base = { .think = systray_think, .draw = systray_draw },
        .padding_x = 5
    },
    NULL
};

/* The monitor that should host the system tray (X11 only allows it on one) */
#define SYSTRAY_MONITOR "DisplayPort-2"

#endif /* CONFIG_H */
