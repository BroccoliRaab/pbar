#ifndef PBAR_H
#define PBAR_H

#include <stdint.h>
#include <time.h>

struct pbar_output_t;

typedef struct pbar_element_t {
    /* think now takes the output so elements can resize based on the monitor */
    void (*think)(struct pbar_element_t *el, struct pbar_output_t *out); 
    void (*draw)(struct pbar_element_t *el, struct pbar_output_t *out, int x);
    int width;
} pbar_element_t;

typedef struct {
    pbar_element_t base;
    char *text;
    uint32_t color;
    int padding_x;
} text_element_t;

typedef struct {
    text_element_t text_base;
    char *badge;
    char *payload;
    char buffer[256];
} label_element_t;

typedef struct {
    text_element_t text_base;
    char *format;
    char buffer[64];
} clock_element_t;

typedef struct {
    text_element_t text_base;
    char *cmd;
    int interval_sec;
    time_t last_run;
    char buffer[256];
} exec_element_t;

typedef struct {
    pbar_element_t base;
    uint32_t color;
    int rect_width;
} rect_element_t;

typedef struct {
    pbar_element_t base;
    int padding_x;
} systray_element_t;

typedef struct pbar_output_t {
    int id;
    int x;
    int y;
    int width;
    int height;
    
    int shm_fd[2];
    uint32_t *shm_pixels[2];
    int shm_size;
    int current_buf;
    uint32_t *pixels;
    
    void *b_state;
    struct pbar_output_t *next;
} pbar_output_t;

static inline uint32_t blend_pixel(uint32_t bg, uint32_t fg, uint8_t alpha) {
    if (alpha == 0) return bg;
    if (alpha == 255) return fg;
    
    uint8_t bg_r = (bg >> 16) & 0xFF;
    uint8_t bg_g = (bg >> 8) & 0xFF;
    uint8_t bg_b = bg & 0xFF;
    
    uint8_t fg_r = (fg >> 16) & 0xFF;
    uint8_t fg_g = (fg >> 8) & 0xFF;
    uint8_t fg_b = fg & 0xFF;
    
    uint8_t r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
    uint8_t g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
    uint8_t b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
    
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

extern pbar_element_t* left_elements[];
extern pbar_element_t* middle_elements[];
extern pbar_element_t* right_elements[];

void label_think(pbar_element_t *el, pbar_output_t *out);
void label_draw(pbar_element_t *el, pbar_output_t *out, int x);
void clock_think(pbar_element_t *el, pbar_output_t *out);
void clock_draw(pbar_element_t *el, pbar_output_t *out, int x);
void exec_think(pbar_element_t *el, pbar_output_t *out);
void exec_draw(pbar_element_t *el, pbar_output_t *out, int x);
void rect_think(pbar_element_t *el, pbar_output_t *out);
void rect_draw(pbar_element_t *el, pbar_output_t *out, int x);
void systray_think(pbar_element_t *el, pbar_output_t *out);
void systray_draw(pbar_element_t *el, pbar_output_t *out, int x);

#endif /* PBAR_H */
