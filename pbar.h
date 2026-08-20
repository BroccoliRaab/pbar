#ifndef PBAR_H
#define PBAR_H

#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include "config.h"

/* --- Core Types & Forward Declarations --- */
typedef struct pbar_output_s pbar_output_t;
typedef struct pbar_element_s pbar_element_t;

typedef void (*pbar_think)(pbar_element_t *el);
typedef void (*pbar_draw)(pbar_element_t *el, pbar_output_t *out, int x_offset);

/* 1. Base Element Struct */
struct pbar_element_s {
    pbar_think think;
    pbar_draw draw;
    uint32_t width;
};

/* 2. Base Text Element (Inherits from pbar_element_t) */
typedef struct {
    pbar_element_t base;   /* MUST be first */
    const char *text;
    uint32_t color;
    int padding_x;
} pbar_text_t;

/* --- Specialized / Derived Element Structs --- */
typedef struct { pbar_element_t base; uint32_t rect_width; uint32_t color; } rect_element_t;
typedef struct { pbar_element_t base; uint32_t padding_x; } systray_element_t;
typedef struct { pbar_text_t text_base; const char *format; char buffer[128]; } clock_element_t;
typedef struct { pbar_text_t text_base; const char *cmd; uint32_t interval_sec; time_t last_run; char buffer[256]; } exec_element_t;
typedef struct { pbar_text_t text_base; const char *badge; const char *payload; char buffer[256]; } label_element_t;

/* --- Output State --- */
struct pbar_output_s {
    int id;
    int x, y;
    int width, height;
    
    uint32_t *pixels;       /* Pointer to the currently active drawing buffer */
    
    /* Zero-copy SHM Double Buffering */
    int shm_fd[2];
    uint32_t *shm_pixels[2];
    int current_buf;        /* 0 or 1 */
    int shm_size;

    void *b_state;
    pbar_output_t *next;
};

/* --- Globals --- */
extern volatile sig_atomic_t running;
extern pbar_output_t *outputs;
extern int output_count;
extern pbar_element_t *elements[];

/* --- Inline Pixel Blending Helper --- */
static inline uint32_t blend_pixel(uint32_t bg, uint32_t fg, uint8_t coverage) {
    uint32_t fg_a = (fg >> 24) & 0xFF;
    if (fg_a == 0) fg_a = 255; 
    
    uint32_t alpha = (fg_a * coverage) / 255;
    if (alpha == 0) return bg;
    if (alpha == 255) return fg;

    uint32_t inv_a = 255 - alpha;
    uint32_t r = (((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * inv_a) / 255;
    uint32_t g = (((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * inv_a) / 255;
    uint32_t b = (((fg) & 0xFF) * alpha + ((bg & 0xFF) * inv_a)) / 255;

    return (0xFF000000) | (r << 16) | (g << 8) | b;
}

/* --- Common Core API --- */
void die(const char *msg);
int  init_shm(pbar_output_t *out);
void draw_output(pbar_output_t *out);

/* --- Backend Abstraction Contract --- */
static int      backend_run(void);
static uint32_t backend_systray_get_width(void);
static void     backend_systray_draw(pbar_output_t *out, int x_offset, uint32_t width);

#endif /* PBAR_H */
