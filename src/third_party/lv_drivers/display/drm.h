/**
 * @file drm.h
 *
 */

#ifndef DRM_H
#define DRM_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#ifndef LV_DRV_NO_CONF
#ifdef LV_CONF_INCLUDE_SIMPLE
#include "lv_drv_conf.h"
#else
#include "../../lv_drv_conf.h"
#endif
#endif

#if USE_DRM

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#if USE_RGA
#include <rga/im2d.h>
#include <rga/rga.h>
#include <rga/RgaApi.h>
#endif
/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/
int drm_init(int bpp);
void drm_disp_drv_deinit(void);
void drm_disp_drv_init(int hor_res, int ver_res, int rot);
int drm_exit(void);
void drm_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p);


/**********************
 *      MACROS
 **********************/

#endif  /*USE_DRM*/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*DRM_H*/
