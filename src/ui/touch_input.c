/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：touch_input.c
 * 文件功能：Goodix 电容触摸屏输入驱动实现
 *           —— 读 /dev/input/event1 MT Type B 协议，注册 LVGL pointer
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <linux/input.h>

#include "touch_input.h"

/* 触摸设备节点（Goodix 电容屏，/proc/bus/input/devices 确认） */
#define TOUCH_DEV "/dev/input/event1"

/* 面板物理分辨率：MIPI-DSI 竖装面板 720×1280 */
#define PANEL_PHYS_W 720
#define PANEL_PHYS_H 1280

/* 触摸文件描述符（O_NONBLOCK，read_cb 由 LVGL 轮询调用不阻塞） */
static int s_touch_fd = -1;

/* 最近一次触摸状态（MT 协议解析结果） */
static int s_phys_x = 0;
static int s_phys_y = 0;
static bool s_is_pressed = false;
static bool s_has_touch = false;

/*****************************************************************************
 * 函数名称：touch_read_cb
 * 功能描述：LVGL 输入设备读取回调——解析 MT 事件并做旋转映射
 * 输入参数：@drv  - 输入设备驱动
 * 输出参数：@data - 填入当前触摸点与按压状态
 * 注意事项：1. 一次 read_cb 读空全部 pending 事件，取最后一帧状态
 *           2. 旋转映射：面板竖装 720×1280，DRM 顺时针旋转 90° 后
 *              逻辑坐标为 1280×720（四角标定实测确认）：
 *              逻辑 X = 物理 Y
 *              逻辑 Y = 面板宽 - 1 - 物理 X
 *****************************************************************************/
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    struct input_event ev;

    (void)drv;

    /* 读空全部 pending 事件（非阻塞，无事件时 read 返回 -1） */
    while (read(s_touch_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (EV_ABS == ev.type) {
            if (ABS_MT_POSITION_X == ev.code) {
                s_phys_x = ev.value;
                s_has_touch = true;
            } else if (ABS_MT_POSITION_Y == ev.code) {
                s_phys_y = ev.value;
                s_has_touch = true;
            } else if (ABS_MT_TRACKING_ID == ev.code) {
                /* value >= 0 按下（新触点），-1 抬起 */
                s_is_pressed = (0 <= ev.value);
                s_has_touch = true;
            }
        }
    }

    /* 顺时针 90° 映射：物理(720×1280) → 逻辑(1280×720) */
    data->point.x = (lv_coord_t)s_phys_y;
    data->point.y = (lv_coord_t)(PANEL_PHYS_W - 1 - s_phys_x);
    data->state = s_is_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

/*****************************************************************************
 * 函数名称：touch_input_init
 * 功能描述：打开触摸设备并注册 LVGL 输入设备
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：驱动注册的 indev 生命周期与进程一致，无需注销
 *****************************************************************************/
int touch_input_init(void)
{
    static lv_indev_drv_t indev_drv;

    s_touch_fd = open(TOUCH_DEV, O_RDONLY | O_NONBLOCK);
    if (0 > s_touch_fd) {
        perror("[UI] 打开触摸设备失败");
        return -1;
    }

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    printf("[INFO] 触摸输入已注册: %s\n", TOUCH_DEV);
    return 0;
}

/*****************************************************************************
 * 函数名称：touch_input_get_last
 * 功能描述：获取最近一次触摸的逻辑坐标（调试/标定用）
 * 输出参数：@x - 逻辑 X 坐标
 *           @y - 逻辑 Y 坐标
 * 返回值：  有触摸数据返回 true，从未收到触摸返回 false
 *****************************************************************************/
bool touch_input_get_last(lv_coord_t *x, lv_coord_t *y)
{
    if ((NULL == x) || (NULL == y) || (false == s_has_touch)) {
        return false;
    }

    *x = (lv_coord_t)s_phys_y;
    *y = (lv_coord_t)(PANEL_PHYS_W - 1 - s_phys_x);
    return true;
}
