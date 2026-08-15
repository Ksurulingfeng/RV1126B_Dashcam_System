/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：ui_main.h
 * 文件功能：LVGL 图形界面 —— 状态栏 + 录像指示（framebuffer 后端）
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#ifndef UI_MAIN_H
#define UI_MAIN_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

#include "detect_share.h"
#include "file_mgr.h"
#include "gps_worker.h"
#include "preview_share.h"

/* UI 线程上下文 */
typedef struct {
    volatile bool   *running;         /* 退出标志 */
    file_mgr_t      *file_mgr;        /* 文件管理器（显示存储状态） */
    gps_worker_t    *gps;             /* GPS 数据（显示定位状态） */
    preview_share_t *preview_share;   /* 预览帧源（GStreamer，直接显示） */
    detect_share_t  *detect_share;    /* 检测结果（AI，叠加画框） */
    lv_font_t       *font;            /* 中文字体（UI 线程初始化后填写） */
} ui_worker_t;

/*****************************************************************************
 * 函数名称：ui_worker_entry
 * 功能描述：UI 线程入口（pthread_create 用）
 * 输入参数：@arg - ui_worker_t 指针
 * 返回值：  NULL
 * 注意事项：fb0 被其他显示程序占用时需先停止（systemui）
 *****************************************************************************/
void *ui_worker_entry(void *arg);

/*****************************************************************************
 * 函数名称：ui_nav_bar_create
 * 功能描述：创建右侧竖排导航栏（各页面共用组件）
 * 输入参数：@parent - 父对象（页面 screen）
 *           @ui     - UI 上下文（按钮回调）
 * 注意事项：在 UI 线程内、页面创建时调用
 *****************************************************************************/
void ui_nav_bar_create(lv_obj_t *parent, ui_worker_t *ui);

#endif /* UI_MAIN_H */
