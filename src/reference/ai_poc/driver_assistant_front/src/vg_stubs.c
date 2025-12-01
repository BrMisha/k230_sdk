/**
 * @file vg_stubs.c
 * @brief Stub implementations for VG-Lite functions referenced in LVGL
 *
 * LVGL 9.3.0 has debug code in lv_draw_buf.c that unconditionally calls
 * vg_allocate_buffer() and vg_free_buffer(). These stubs satisfy the linker.
 * The actual code path uses the callback-based allocation instead.
 */

#include <stdlib.h>

void *vg_allocate_buffer(size_t size) {
    return malloc(size);
}

void vg_free_buffer(void *p) {
    free(p);
}