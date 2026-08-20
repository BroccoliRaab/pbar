#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <fcntl.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "pbar.h"
#include "config.h"

/* --- Backend API Declarations --- */
uint32_t backend_systray_get_width(pbar_output_t *out);
void backend_systray_draw(pbar_output_t *out, int x);
int backend_run(void);

volatile sig_atomic_t running = 1;
pbar_output_t *outputs = NULL;
int output_count = 0;

/* --- STB TrueType Globals --- */
static unsigned char *font_buffer = NULL;
static stbtt_fontinfo font_info;
static float font_scale;
static int font_ascent, font_descent, font_line_gap;

void die(const char *msg) {
    fprintf(stderr, "fatal: %s\n", msg);
    exit(EXIT_FAILURE);
}

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

void pbar_font_init(const char *filename, float size) {
    FILE *f = fopen(filename, "rb");
    if (!f) die("Failed to open font file.");
    
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    font_buffer = malloc(sz);
    if (fread(font_buffer, 1, sz, f) != (size_t)sz) die("Failed to read font file.");
    fclose(f);
    
    if (!stbtt_InitFont(&font_info, font_buffer, stbtt_GetFontOffsetForIndex(font_buffer, 0))) {
        die("Failed to initialize font.");
    }
    
    font_scale = stbtt_ScaleForPixelHeight(&font_info, size);
    stbtt_GetFontVMetrics(&font_info, &font_ascent, &font_descent, &font_line_gap);
}

int measure_text_width(const char *text) {
    if (!text) return 0;
    float w = 0.0f;
    for (int i = 0; text[i]; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font_info, text[i], &advance, &lsb);
        w += advance * font_scale;
        if (text[i+1]) {
            w += font_scale * stbtt_GetCodepointKernAdvance(&font_info, text[i], text[i+1]);
        }
    }
    return (int)w;
}

static void draw_text(pbar_output_t *out, int x, const char *text, uint32_t color) {
    if (!text || !out || !out->pixels) return;

    int text_height = (int)((font_ascent - font_descent) * font_scale);
    int y_offset = (BAR_HEIGHT - text_height) / 2;
    int baseline = y_offset + (int)(font_ascent * font_scale);
    float xpos = (float)x;

    for (int i = 0; text[i]; i++) {
        int advance, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&font_info, text[i], &advance, &lsb);
        stbtt_GetCodepointBitmapBox(&font_info, text[i], font_scale, font_scale, &x0, &y0, &x1, &y1);
        
        int bw = x1 - x0;
        int bh = y1 - y0;
        
        if (bw > 0 && bh > 0) {
            unsigned char *bitmap = malloc(bw * bh);
            stbtt_MakeCodepointBitmap(&font_info, bitmap, bw, bh, bw, font_scale, font_scale, text[i]);
            
            int cur_x = (int)xpos + x0;
            int cur_y = baseline + y0;
            
            for (int row = 0; row < bh; row++) {
                for (int col = 0; col < bw; col++) {
                    int px = cur_x + col;
                    int py = cur_y + row;
                    if (px >= 0 && px < out->width && py >= 0 && py < out->height) {
                        uint8_t alpha = bitmap[row * bw + col];
                        if (alpha > 0) {
                            int idx = py * out->width + px;
                            out->pixels[idx] = blend_pixel(out->pixels[idx], color, alpha);
                        }
                    }
                }
            }
            free(bitmap);
        }
        
        xpos += (advance * font_scale);
        if (text[i+1]) {
            xpos += font_scale * stbtt_GetCodepointKernAdvance(&font_info, text[i], text[i+1]);
        }
    }
}

int init_shm(pbar_output_t *out) {
    out->shm_size = out->width * out->height * 4;
    out->current_buf = 0;

    for (int i = 0; i < 2; i++) {
        out->shm_fd[i] = memfd_create("pbar_pixels", MFD_CLOEXEC);
        if (out->shm_fd[i] < 0) return -1;
        if (ftruncate(out->shm_fd[i], out->shm_size) < 0) return -1;

        out->shm_pixels[i] = mmap(NULL, out->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, out->shm_fd[i], 0);
        if (out->shm_pixels[i] == MAP_FAILED) return -1;
        memset(out->shm_pixels[i], 0, out->shm_size);
    }
    out->pixels = out->shm_pixels[0];
    return 0;
}

void draw_output(pbar_output_t *out) {
    if (!out || out->width <= 0 || out->height <= 0) return;

    out->current_buf = (out->current_buf + 1) % 2;
    out->pixels = out->shm_pixels[out->current_buf];

    for (int i = 0; i < out->width * out->height; i++) {
        out->pixels[i] = COLOR_BG;
    }

    int left_w = 0, mid_w = 0, right_w = 0;

    for (pbar_element_t **el = left_elements; *el; el++) {
        if ((*el)->think) (*el)->think(*el, out);
        left_w += (*el)->width;
    }
    for (pbar_element_t **el = middle_elements; *el; el++) {
        if ((*el)->think) (*el)->think(*el, out);
        mid_w += (*el)->width;
    }
    for (pbar_element_t **el = right_elements; *el; el++) {
        if ((*el)->think) (*el)->think(*el, out);
        right_w += (*el)->width;
    }

    int x_offset = 0;

    for (pbar_element_t **el = left_elements; *el; el++) {
        if ((*el)->draw) (*el)->draw(*el, out, x_offset);
        x_offset += (*el)->width;
    }

    x_offset = (out->width / 2) - (mid_w / 2);
    if (x_offset < left_w) x_offset = left_w;
    
    for (pbar_element_t **el = middle_elements; *el; el++) {
        if ((*el)->draw) (*el)->draw(*el, out, x_offset);
        x_offset += (*el)->width;
    }

    x_offset = out->width - right_w;
    if (x_offset < (out->width / 2) + (mid_w / 2)) {
        x_offset = (out->width / 2) + (mid_w / 2);
    }

    for (pbar_element_t **el = right_elements; *el; el++) {
        if ((*el)->draw) (*el)->draw(*el, out, x_offset);
        x_offset += (*el)->width;
    }
}

/* --- Element Implementations --- */

void label_think(pbar_element_t *el, pbar_output_t *out) {
    (void)out;
    label_element_t *lbl = (label_element_t *)el;
    if (lbl->badge && lbl->payload) {
        snprintf(lbl->buffer, sizeof(lbl->buffer), "%s %s", lbl->badge, lbl->payload);
        lbl->text_base.text = lbl->buffer;
    }
    if (lbl->text_base.text) {
        el->width = measure_text_width(lbl->text_base.text) + (lbl->text_base.padding_x * 2);
    }
}

void label_draw(pbar_element_t *el, pbar_output_t *out, int x_offset) {
    label_element_t *lbl = (label_element_t *)el;
    if (lbl->text_base.text) draw_text(out, x_offset + lbl->text_base.padding_x, lbl->text_base.text, lbl->text_base.color);
}

void clock_think(pbar_element_t *el, pbar_output_t *out) {
    (void)out;
    clock_element_t *clk = (clock_element_t *)el;
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    if (tm_info && clk->format) {
        strftime(clk->buffer, sizeof(clk->buffer), clk->format, tm_info);
        clk->text_base.text = clk->buffer;
    }
    if (clk->text_base.text) {
        el->width = measure_text_width(clk->text_base.text) + (clk->text_base.padding_x * 2);
    }
}

void clock_draw(pbar_element_t *el, pbar_output_t *out, int x_offset) {
    clock_element_t *clk = (clock_element_t *)el;
    if (clk->text_base.text) draw_text(out, x_offset + clk->text_base.padding_x, clk->text_base.text, clk->text_base.color);
}

void exec_think(pbar_element_t *el, pbar_output_t *out) {
    (void)out;
    exec_element_t *ex = (exec_element_t *)el;
    time_t now = time(NULL);
    if (now - ex->last_run >= ex->interval_sec && ex->cmd) {
        FILE *fp = popen(ex->cmd, "r");
        if (fp) {
            if (fgets(ex->buffer, sizeof(ex->buffer), fp)) {
                ex->buffer[strcspn(ex->buffer, "\n")] = 0;
                ex->text_base.text = ex->buffer;
            }
            pclose(fp);
        }
        ex->last_run = now;
    }
    if (ex->text_base.text) el->width = measure_text_width(ex->text_base.text) + (ex->text_base.padding_x * 2);
}

void exec_draw(pbar_element_t *el, pbar_output_t *out, int x_offset) {
    exec_element_t *ex = (exec_element_t *)el;
    if (ex->text_base.text) draw_text(out, x_offset + ex->text_base.padding_x, ex->text_base.text, ex->text_base.color);
}

void rect_think(pbar_element_t *el, pbar_output_t *out) { 
    (void)out;
    el->width = ((rect_element_t *)el)->rect_width; 
}

void rect_draw(pbar_element_t *el, pbar_output_t *out, int x_offset) {
    rect_element_t *rect = (rect_element_t *)el;
    for (int y = 4; y < out->height - 4; y++) {
        for (int x = 0; x < rect->rect_width; x++) {
            int px = x_offset + x;
            if (px >= 0 && px < out->width) {
                out->pixels[y * out->width + px] = blend_pixel(out->pixels[y * out->width + px], rect->color, 255);
            }
        }
    }
}

void systray_think(pbar_element_t *el, pbar_output_t *out) {
    systray_element_t *st = (systray_element_t *)el;
    uint32_t tray_w = backend_systray_get_width(out); // Width returns 0 on non-tray monitors
    el->width = (tray_w > 0) ? (tray_w + st->padding_x * 2) : 0;
}

void systray_draw(pbar_element_t *el, pbar_output_t *out, int x_offset) {
    systray_element_t *st = (systray_element_t *)el;
    if (el->width <= 0) return;
    
    backend_systray_draw(out, x_offset + st->padding_x);
}

#ifdef BUILD_X11
#include "backend_x11.h"
#endif

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    pbar_font_init(FONT_PATH, FONT_SIZE);

    return backend_run();
}
