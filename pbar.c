#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>

#include "pbar.h"

/* --- Globals --- */
bool running = true;
pbar_output_t *outputs = NULL;
int output_count = 0;

void die(const char *msg) {
    fprintf(stderr, "fatal: %s\n", msg);
}

static void handle_signal(int sig) {
    (void)sig;
    running = false;
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

void draw_output(pbar_output_t *out) {
    if (!out->pixels) return;
    for (int y = 0; y < out->height; y++) {
        for (int x = 0; x < out->width; x++) {
            if (x > out->width / 2 - 50 && x < out->width / 2 + 50 && y > 10 && y < 22) {
                out->pixels[y * out->width + x] = COLOR_FG;
            } else {
                out->pixels[y * out->width + x] = COLOR_BG;
            }
        }
    }
}

/* --- Display Server Injection --- */
#if defined(BUILD_WAYLAND)
    #include "backend_wayland.h"
#elif defined(BUILD_X11)
    #include "backend_x11.h"
#else
    #error "You must define either BUILD_WAYLAND or BUILD_X11 during compilation"
#endif

int main(void) {
    int r = -1;

    struct sigaction sa = { .sa_handler = handle_signal };
    if (sigaction(SIGINT, &sa, NULL) < 0) goto out;
    if (sigaction(SIGTERM, &sa, NULL) < 0) goto out;

    if (backend_run() < 0) goto out;

    r = 0;

out:
    return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
