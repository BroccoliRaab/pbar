#ifndef PBAR_H
#define PBAR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* --- Types --- */
typedef struct pbar_output_s pbar_output_t;
struct pbar_output_s {
    int id;
    int x, y;
    int width, height;
    
    /* Shared Memory Pixel Buffer */
    uint32_t *pixels;
    int shm_fd;
    int shm_size;

    /* Backend specific state */
    void *b_state;

    pbar_output_t *next;
};

/* --- Globals --- */
extern bool running;
extern pbar_output_t *outputs;
extern int output_count;

/* --- Common API --- */
void die(const char *msg);
int  init_shm(pbar_output_t *out);
void draw_output(pbar_output_t *out);

/* --- Backend API --- */
static int backend_run(void);

#endif /* PBAR_H */
