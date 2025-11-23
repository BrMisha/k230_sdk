/**
 * @file lv_port_disp.h
 * LVGL display port header
 */

#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Initialize LVGL display port */
void lv_port_disp_init(void);

/* Deinitialize LVGL display port */
void lv_port_disp_deinit(void);

/* Custom tick function for LVGL */
uint32_t custom_tick_get(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LV_PORT_DISP_H */