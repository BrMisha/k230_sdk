/**
 * @file lv_port_disp.c
 * LVGL display port for ST7789V LCD via /dev/fb1 framebuffer
 * 320x170 RGB565
 * LVGL v9.x API
 */

#include "lv_port_disp.h"
#include "lvgl.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*********************
 *      DEFINES
 *********************/
#define DISP_HOR_RES 320
#define DISP_VER_RES 170
#define FBDEV_PATH "/dev/fb1"

/**********************
 *  STATIC VARIABLES
 **********************/
static int fb_fd = -1;
static uint16_t *fb_mapped = NULL;
static size_t fb_size = 0;
static lv_display_t *disp = NULL;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

uint32_t custom_tick_get(void)
{
    static struct timeval start_time = {0, 0};
    static int call_count = 0;
    struct timeval tv;

    gettimeofday(&tv, NULL);

    // Initialize start time on first call
    if (start_time.tv_sec == 0 && start_time.tv_usec == 0) {
        start_time = tv;
    }

    // Return milliseconds since start
    uint64_t elapsed_ms = ((uint64_t)(tv.tv_sec - start_time.tv_sec) * 1000) +
                          ((tv.tv_usec - start_time.tv_usec) / 1000);

    // Debug: Print every 100 calls
    if (++call_count % 100 == 0) {
        printf("custom_tick_get() called %d times, returning %u ms\n", call_count, (uint32_t)elapsed_ms);
    }

    return (uint32_t)elapsed_ms;
}

void lv_port_disp_init(void)
{
    /* Open framebuffer device */
    fb_fd = open(FBDEV_PATH, O_RDWR);
    if (fb_fd < 0) {
        perror("Failed to open framebuffer device");
        return;
    }

    /* Get framebuffer info */
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("Error reading fixed information");
        close(fb_fd);
        fb_fd = -1;
        return;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("Error reading variable information");
        close(fb_fd);
        fb_fd = -1;
        return;
    }

    printf("FB: %dx%d, %d bpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    /* Memory map the framebuffer */
    fb_size = finfo.smem_len;
    fb_mapped = (uint16_t *)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);

    if (fb_mapped == MAP_FAILED) {
        perror("Failed to mmap framebuffer");
        close(fb_fd);
        fb_fd = -1;
        return;
    }

    /* Clear framebuffer */
    memset(fb_mapped, 0, fb_size);

    /* Create LVGL display (LVGL 9.x API) */
    disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    if (!disp) {
        perror("Failed to create display");
        munmap(fb_mapped, fb_size);
        close(fb_fd);
        fb_fd = -1;
        return;
    }

    /* Set display flush callback */
    lv_display_set_flush_cb(disp, disp_flush);

    /* Allocate draw buffers (2 buffers for double buffering) */
    size_t buf_size = DISP_HOR_RES * DISP_VER_RES / 10 * sizeof(lv_color_t);  // 1/10 screen size
    void *buf1 = malloc(buf_size);
    void *buf2 = malloc(buf_size);

    if (!buf1 || !buf2) {
        perror("Failed to allocate draw buffers");
        if (buf1) free(buf1);
        if (buf2) free(buf2);
        lv_display_delete(disp);
        munmap(fb_mapped, fb_size);
        close(fb_fd);
        fb_fd = -1;
        disp = NULL;
        return;
    }

    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    printf("LVGL display port initialized: %dx%d RGB565\n", DISP_HOR_RES, DISP_VER_RES);
}

void lv_port_disp_deinit(void)
{
    if (disp) {
        lv_display_delete(disp);
        disp = NULL;
    }

    if (fb_mapped && fb_mapped != MAP_FAILED) {
        munmap(fb_mapped, fb_size);
        fb_mapped = NULL;
    }

    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void disp_flush(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *px_map)
{
    if (!fb_mapped) {
        lv_display_flush_ready(disp_drv);
        return;
    }

    /* Get buffer as uint16_t pointer (RGB565) */
    uint16_t *color_p = (uint16_t *)px_map;

    /* Calculate area dimensions */
    int32_t width = area->x2 - area->x1 + 1;
    int32_t height = area->y2 - area->y1 + 1;

    /* Copy LVGL buffer to framebuffer line by line */
    int32_t y;
    for (y = 0; y < height; y++) {
        uint16_t *fb_line = fb_mapped + ((area->y1 + y) * DISP_HOR_RES + area->x1);
        memcpy(fb_line, color_p, width * sizeof(uint16_t));
        color_p += width;
    }

    /* Tell LVGL we're done flushing */
    lv_display_flush_ready(disp_drv);
}