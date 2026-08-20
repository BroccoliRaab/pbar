#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "pbar.h"

/* --- Globals --- */
volatile sig_atomic_t running = 1;
pbar_output_t *outputs = NULL;
int output_count = 0;

/* Font State */
static stbtt_fontinfo font_info;
static unsigned char *font_buffer = NULL;
static bool font_loaded = false;

void die(const char *msg) {
    fprintf(stderr, "fatal: %s\n", msg);
    exit(EXIT_FAILURE);
}

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

int init_shm(pbar_output_t *out) {
    int r = -1;

    out->shm_size = out->width * out->height * 4;
    out->shm_fd = memfd_create("pbar_pixels", MFD_CLOEXEC);
    if (out->shm_fd < 0) goto out;

    if (ftruncate(out->shm_fd, out->shm_size) < 0) goto out_close_fd;

    out->pixels = mmap(NULL, out->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, out->shm_fd, 0);
    if (out->pixels == MAP_FAILED) {
        out->pixels = NULL;
        goto out_close_fd;
    }

    memset(out->pixels, 0, out->shm_size);
    r = 0;
    return r;

out_close_fd:
    close(out->shm_fd);
    out->shm_fd = -1;
out:
    return r;
}

/* --- Display Server Injection --- */
#if defined(BUILD_WAYLAND)
    #include "backend_wayland.h"
#elif defined(BUILD_X11)
    #include "backend_x11.h"
#else
    #error "You must define either BUILD_WAYLAND or BUILD_X11 during compilation"
#endif

/* --- Font Initialization Helper --- */
static bool ensure_font_loaded(void) {
    if (font_loaded) return true;

    FILE *f = fopen(FONT_PATH, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    font_buffer = malloc(size);
    if (!font_buffer) {
        fclose(f);
        return false;
    }

    if (fread(font_buffer, 1, size, f) != (size_t)size) {
        free(font_buffer);
        font_buffer = NULL;
        fclose(f);
        return false;
    }
    fclose(f);

    if (!stbtt_InitFont(&font_info, font_buffer, 0)) {
        free(font_buffer);
        font_buffer = NULL;
        return false;
    }

    font_loaded = true;
    return true;
}

/* ========================================================================= */
/* --- Element Handlers & Logic ---                                          */
/* ========================================================================= */

/* --- Base Text Element Handlers --- */
static void text_think(pbar_element_t *el) {
    pbar_text_t *self = (pbar_text_t *)el;
    
    if (!ensure_font_loaded() || !self->text || self->text[0] == '\0') {
        self->base.width = 0;
        return;
    }

    float scale = stbtt_ScaleForPixelHeight(&font_info, FONT_SIZE);
    float x_advance = 0.0f;

    for (int i = 0; self->text[i] != '\0'; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font_info, self->text[i], &advance, &lsb);
        x_advance += advance * scale;

        if (self->text[i + 1] != '\0') {
            int kern = stbtt_GetCodepointKernAdvance(&font_info, self->text[i], self->text[i + 1]);
            x_advance += kern * scale;
        }
    }

    self->base.width = (uint32_t)(x_advance) + (self->padding_x * 2);
}

static void text_draw(pbar_element_t *el, pbar_output_t *out, int x_offset) {
    pbar_text_t *self = (pbar_text_t *)el;
    if (!font_loaded || !self->text || self->base.width == 0) return;

    float scale = stbtt_ScaleForPixelHeight(&font_info, FONT_SIZE);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);

    /* Restored exact baseline formula from 3 iterations ago */
    int baseline = (out->height + (int)((ascent + descent) * scale)) / 2;
    float xpos = (float)(x_offset + self->padding_x);

    for (int i = 0; self->text[i] != '\0'; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font_info, self->text[i], &advance, &lsb);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&font_info, self->text[i], scale, scale, &x0, &y0, &x1, &y1);

        int glyph_w = x1 - x0;
        int glyph_h = y1 - y0;

        if (glyph_w > 0 && glyph_h > 0) {
            unsigned char *bmp = malloc(glyph_w * glyph_h);
            if (bmp) {
                stbtt_MakeCodepointBitmap(&font_info, bmp, glyph_w, glyph_h, glyph_w, scale, scale, self->text[i]);

                int gx = (int)xpos + x0;
                int gy = baseline + y0;

                for (int row = 0; row < glyph_h; row++) {
                    int py = gy + row;
                    if (py < 0 || py >= out->height) continue;

                    for (int col = 0; col < glyph_w; col++) {
                        int px = gx + col;
                        if (px < x_offset || px >= out->width || px >= (int)(x_offset + self->base.width)) continue;

                        uint8_t alpha = bmp[row * glyph_w + col];
                        if (alpha == 0) continue;

                        uint32_t *dst = &out->pixels[py * out->width + px];
                        *dst = blend_pixel(*dst, self->color, alpha);
                    }
                }
                free(bmp);
            }
        }

        xpos += advance * scale;

        if (self->text[i + 1] != '\0') {
            int kern = stbtt_GetCodepointKernAdvance(&font_info, self->text[i], self->text[i + 1]);
            xpos += kern * scale;
        }
    }
}

/* --- Rectangle Element Handlers --- */
static void rect_think(pbar_element_t *el) {
    rect_element_t *self = (rect_element_t *)el;
    self->base.width = self->rect_width;
}

static void rect_draw(pbar_element_t *el, pbar_output_t *out, int x_offset) {
    rect_element_t *self = (rect_element_t *)el;
    if (!out || self->base.width == 0) return;

    for (int y = 0; y < out->height; y++) {
        for (uint32_t x = 0; x < self->base.width && (x_offset + x) < (uint32_t)out->width; x++) {
            out->pixels[y * out->width + (x_offset + x)] = self->color;
        }
    }
}

/* --- Systray Element Handlers --- */
static void systray_think(pbar_element_t *el) {
    systray_element_t *self = (systray_element_t *)el;
    uint32_t tray_w = backend_systray_get_width();
    self->base.width = (tray_w > 0) ? (tray_w + self->padding_x * 2) : 0;
}

static void systray_draw(pbar_element_t *el, pbar_output_t *out, int x_offset) {
    systray_element_t *self = (systray_element_t *)el;
    if (!out || self->base.width == 0) return;

    for (int y = 0; y < out->height; y++) {
        for (uint32_t x = 0; x < self->base.width && (x_offset + x) < (uint32_t)out->width; x++) {
            out->pixels[y * out->width + (x_offset + x)] = COLOR_BG;
        }
    }

    backend_systray_draw(out, x_offset + self->padding_x, self->base.width - (self->padding_x * 2));
}

/* --- Derived Text Element Handlers --- */

/* 1. Clock Element Handler */
static void clock_think(pbar_element_t *el) {
    clock_element_t *self = (clock_element_t *)el;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    if (tm_info && self->format) {
        strftime(self->buffer, sizeof(self->buffer), self->format, tm_info);
        self->text_base.text = self->buffer;
    }

    text_think(el);
}

/* 2. Exec Process Element Handler */
static void exec_think(pbar_element_t *el) {
    exec_element_t *self = (exec_element_t *)el;
    time_t now = time(NULL);

    if (self->cmd && (self->interval_sec == 0 || (now - self->last_run) >= (time_t)self->interval_sec)) {
        FILE *fp = popen(self->cmd, "r");
        if (fp) {
            if (fgets(self->buffer, sizeof(self->buffer), fp)) {
                size_t len = strlen(self->buffer);
                if (len > 0 && self->buffer[len - 1] == '\n') {
                    self->buffer[len - 1] = '\0';
                }
                self->text_base.text = self->buffer;
            } else {
                self->text_base.text = "";
            }
            pclose(fp);
        }
        self->last_run = now;
    }

    text_think(el);
}

/* 3. Label Element Handler */
static void label_think(pbar_element_t *el) {
    label_element_t *self = (label_element_t *)el;
    
    /* Fixed: Reads from self->payload instead of self->buffer to prevent overlap undefined behavior */
    snprintf(self->buffer, sizeof(self->buffer), "[%s] %s", 
             self->badge ? self->badge : "SYS", 
             self->payload ? self->payload : "");
    self->text_base.text = self->buffer;

    text_think(el);
}

/* ========================================================================= */
/* --- Element Registry (Safe Static Declarations) ---                      */
/* ========================================================================= */

/* Moving these from file-scope compound literals to static objects prevents 
   segfaults from compiler placing mutable buffers into .rodata */

static pbar_text_t el_text = {
    .base = { .think = text_think, .draw = text_draw, .width = 0 },
    .text = "pbar v1.0",
    .color = COLOR_TEXT,
    .padding_x = 8
};

static rect_element_t el_rect = {
    .base = { .think = rect_think, .draw = rect_draw, .width = 0 },
    .rect_width = 2,
    .color = 0xFF555555
};

static label_element_t el_label = {
    .text_base = { .base = { .think = label_think, .draw = text_draw, .width = 0 }, .color = COLOR_TEXT, .padding_x = 8 },
    .badge = "HOST",
    .payload = "X11 Display",
    .buffer = {0}
};

static exec_element_t el_exec = {
    .text_base = { .base = { .think = exec_think, .draw = text_draw, .width = 0 }, .color = COLOR_TEXT, .padding_x = 8 },
    .cmd = "uname -r",
    .interval_sec = 60,
    .last_run = 0,
    .buffer = {0}
};

static clock_element_t el_clock = {
    .text_base = { .base = { .think = clock_think, .draw = text_draw, .width = 0 }, .color = COLOR_TEXT, .padding_x = 8 },
    .format = "%b %d %H:%M",
    .buffer = {0}
};

static systray_element_t el_tray = {
    .base = { .think = systray_think, .draw = systray_draw, .width = 0 },
    .padding_x = 4
};

pbar_element_t *elements[] = {
    (pbar_element_t *)&el_text,
    (pbar_element_t *)&el_rect,
    (pbar_element_t *)&el_label,
    (pbar_element_t *)&el_rect,
    (pbar_element_t *)&el_exec,
    (pbar_element_t *)&el_rect,
    (pbar_element_t *)&el_clock,
    (pbar_element_t *)&el_tray,
    NULL
};

/* ========================================================================= */
/* --- Immediate Mode Drawing System ---                                     */
/* ========================================================================= */

void draw_output(pbar_output_t *out) {
    if (!out || !out->pixels || out->width <= 0 || out->height <= 0) return;

    /* Clear canvas */
    for (int i = 0; i < out->width * out->height; i++) {
        out->pixels[i] = COLOR_BG;
    }

    /* 1. Think Pass */
    for (pbar_element_t **el = elements; *el != NULL; el++) {
        if ((*el)->think) {
            (*el)->think(*el);
        }
    }

    /* 2. Render Pass */
    int x_offset = 0;
    for (pbar_element_t **el = elements; *el != NULL; el++) {
        if ((*el)->draw) {
            (*el)->draw(*el, out, x_offset);
        }
        
        x_offset += (*el)->width;
        if (x_offset >= out->width) break; 
    }
}

int main(void) {
    int r = -1;

    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0) goto out;
    if (sigaction(SIGTERM, &sa, NULL) < 0) goto out;

    if (backend_run() < 0) goto out;

    r = 0;

out:
    if (font_buffer) free(font_buffer);
    return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
