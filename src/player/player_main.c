/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：player_main.c
 * 文件功能：录像回放独立进程（LVGL 界面版）
 *           硬解 + 缩放成 BGRA 帧，appsink 送 LVGL canvas 全屏显示，
 *           叠加暂停/退出按钮（触摸点击控制）；
 *           文件列表逐个播放，全部播完自动退出，SIGTERM 可随时终止
 * 作    者：heifast
 * 创建日期：2026-08-17
 *****************************************************************************/

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <lvgl.h>
#include "display/drm.h"

#include "log.h"
#include "touch_input.h"

/* 模块日志标签 */
#define TAG_PLAYER "PLY"

/* 播放管线（appsink 模式）：
 * mppvideodec 硬解 → videoconvert(RGA 转 BGRA) → videoscale 缩到
 * 逻辑屏 1280×720（LVGL 逻辑横屏，与主程序一致，视频等比全屏） */
#define PLAYER_LAUNCH_STR \
    "filesrc location=%s ! qtdemux ! h264parse ! mppvideodec " \
    "! videoconvert ! videoscale ! video/x-raw,format=BGRA," \
    "width=1280,height=720 ! appsink name=sink"

/* 逻辑屏幕尺寸（drm_disp_drv_init 旋转 90° 后，与主程序一致） */
#define PLAYER_SCREEN_W 1280
#define PLAYER_SCREEN_H 720

/* 按钮尺寸（触摸友好 ≥88px） */
#define PLAYER_BTN_SIZE 88

/* 信号处理置位：请求停止播放 */
static volatile sig_atomic_t s_is_stop_requested = 0;

/* 播放状态 */
static GstElement *s_pipeline = NULL; /* 当前播放管线（按钮回调用） */
static GstAppSink *s_appsink = NULL;  /* appsink（拉帧用） */
static bool s_is_paused = false;      /* 是否暂停 */

/* LVGL 界面 */
static lv_obj_t *s_canvas;          /* 全屏视频 canvas（底层） */
static lv_color_t *s_canvas_buf;    /* canvas 帧缓冲（BGRA 直拷） */
static lv_obj_t *s_btn_pause;       /* 暂停/继续按钮 */
static lv_obj_t *s_btn_pause_label; /* 按钮图标（暂停/播放切换） */

/*****************************************************************************
 * 函数名称：on_sigterm
 * 功能描述：SIGTERM/SIGINT 处理——置停止标志，主循环随即退出
 * 输入参数：@sig - 信号编号（不使用）
 *****************************************************************************/
static void on_sigterm(int sig)
{
    (void)sig;
    s_is_stop_requested = 1;
}

/*****************************************************************************
 * 函数名称：btn_exit_cb
 * 功能描述：退出按钮点击——置停止标志结束播放
 * 输入参数：@e - LVGL 事件（不使用）
 *****************************************************************************/
static void btn_exit_cb(lv_event_t *e)
{
    (void)e;
    LOG_I(TAG_PLAYER, "点击退出");
    s_is_stop_requested = 1;
}

/*****************************************************************************
 * 函数名称：btn_pause_cb
 * 功能描述：暂停/继续按钮点击——切换管线状态与按钮图标
 * 输入参数：@e - LVGL 事件（不使用）
 *****************************************************************************/
static void btn_pause_cb(lv_event_t *e)
{
    (void)e;

    if (NULL == s_pipeline) {
        return;
    }
    if (s_is_paused) {
        (void)gst_element_set_state(s_pipeline, GST_STATE_PLAYING);
        s_is_paused = false;
        lv_label_set_text(s_btn_pause_label, LV_SYMBOL_PAUSE);
        LOG_I(TAG_PLAYER, "继续");
    } else {
        (void)gst_element_set_state(s_pipeline, GST_STATE_PAUSED);
        s_is_paused = true;
        lv_label_set_text(s_btn_pause_label, LV_SYMBOL_PLAY);
        LOG_I(TAG_PLAYER, "已暂停");
    }
}

/*****************************************************************************
 * 函数名称：ui_build
 * 功能描述：构建播放界面——全屏 canvas + 右上退出 + 右下暂停按钮
 * 注意事项：按钮浮于 canvas 上层；canvas 缓冲外部 malloc（LVGL 池够用）
 *****************************************************************************/
static void ui_build(void)
{
    lv_obj_t *btn;
    lv_obj_t *label;

    /* 全屏 canvas 垫底（先创建保证 z 序最底） */
    s_canvas = lv_canvas_create(lv_scr_act());
    s_canvas_buf = (lv_color_t *)malloc(sizeof(lv_color_t) *
                                        PLAYER_SCREEN_W * PLAYER_SCREEN_H);
    if (NULL == s_canvas_buf) {
        LOG_E(TAG_PLAYER, "canvas 缓冲分配失败");
        return;
    }
    memset(s_canvas_buf, 0, sizeof(lv_color_t) *
           PLAYER_SCREEN_W * PLAYER_SCREEN_H);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, PLAYER_SCREEN_W,
                         PLAYER_SCREEN_H, LV_IMG_CF_TRUE_COLOR);

    /* 退出按钮：右上角红底 ✕ */
    btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, PLAYER_BTN_SIZE, PLAYER_BTN_SIZE);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -16, 16);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF2D55), 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_add_event_cb(btn, btn_exit_cb, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    lv_obj_center(label);
    lv_label_set_text(label, LV_SYMBOL_CLOSE);

    /* 暂停/继续按钮：右下角深灰底 */
    s_btn_pause = lv_btn_create(lv_scr_act());
    lv_obj_set_size(s_btn_pause, PLAYER_BTN_SIZE, PLAYER_BTN_SIZE);
    lv_obj_align(s_btn_pause, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_set_style_bg_color(s_btn_pause, lv_color_hex(0x161D26), 0);
    lv_obj_set_style_radius(s_btn_pause, 16, 0);
    lv_obj_add_event_cb(s_btn_pause, btn_pause_cb, LV_EVENT_CLICKED, NULL);
    s_btn_pause_label = lv_label_create(s_btn_pause);
    lv_obj_center(s_btn_pause_label);
    lv_label_set_text(s_btn_pause_label, LV_SYMBOL_PAUSE);
}

/*****************************************************************************
 * 函数名称：pump_frame
 * 功能描述：从 appsink 拉取最新帧（丢旧帧），BGRA 直拷 canvas 并刷新
 * 注意事项：循环拉空缓冲只保留最后一帧，追不上帧率时主动丢帧
 *****************************************************************************/
static void pump_frame(void)
{
    GstSample *sample = NULL;
    GstSample *last = NULL;

    while (NULL != (sample = gst_app_sink_try_pull_sample(s_appsink, 0))) {
        if (NULL != last) {
            gst_sample_unref(last);
        }
        last = sample;
    }
    if (NULL != last) {
        GstBuffer *buf = gst_sample_get_buffer(last);
        GstMapInfo map;

        if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
            memcpy(s_canvas_buf, map.data,
                   sizeof(lv_color_t) * PLAYER_SCREEN_W * PLAYER_SCREEN_H);
            gst_buffer_unmap(buf, &map);
            lv_obj_invalidate(s_canvas);
        }
        gst_sample_unref(last);
    }
}

/*****************************************************************************
 * 函数名称：build_pipeline
 * 功能描述：按 parse-launch 字符串构建单文件播放管线
 * 输入参数：@path - 视频文件路径
 * 返回值：  成功返回管线，失败返回 NULL
 * 注意事项：路径拼接入 launch 串，调用方保证不含空格等特殊字符
 *****************************************************************************/
static GstElement *build_pipeline(const char *path)
{
    GstElement *pipeline = NULL;
    GError *err = NULL;
    char launch[512] = {0};

    (void)snprintf(launch, sizeof(launch), PLAYER_LAUNCH_STR, path);
    pipeline = gst_parse_launch(launch, &err);
    if (NULL == pipeline) {
        LOG_E(TAG_PLAYER, "管线构建失败：%s",
              (NULL != err) ? err->message : "未知错误");
        if (NULL != err) {
            g_error_free(err);
        }
        return NULL;
    }
    return pipeline;
}

/*****************************************************************************
 * 函数名称：play_file
 * 功能描述：播放单个视频文件直至 EOS/错误/外部终止/按钮退出
 * 输入参数：@path - 视频文件路径
 * 返回值：  0 正常播完，-1 播放错误，1 用户/信号终止
 * 注意事项：主循环驱动 LVGL（按钮响应）与 appsink 拉帧
 *****************************************************************************/
static int play_file(const char *path)
{
    GstElement *pipeline = NULL;
    GstBus *bus = NULL;
    bool is_eos = false;
    int ret = 0;

    LOG_I(TAG_PLAYER, "播放：%s", path);
    pipeline = build_pipeline(path);
    if (NULL == pipeline) {
        return -1;
    }

    s_pipeline = pipeline;
    /* 注意：SDK 的 libgstreamer 无 gst_element_get_by_name（1.24 新 API），
     * 用 gst_bin_get_by_name 等价替代 */
    s_appsink = (GstAppSink *)gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    s_is_paused = false;
    s_is_stop_requested = 0;
    memset(s_canvas_buf, 0, sizeof(lv_color_t) *
           PLAYER_SCREEN_W * PLAYER_SCREEN_H);
    lv_obj_invalidate(s_canvas);
    bus = gst_element_get_bus(pipeline);
    (void)gst_element_set_state(pipeline, GST_STATE_PLAYING);

    while ((0 == s_is_stop_requested) && (false == is_eos)) {
        GstMessage *msg = NULL;

        /* 消费 bus 消息：EOS 播完，错误报错 */
        while (NULL != (msg = gst_bus_pop(bus))) {
            if (GST_MESSAGE_EOS == GST_MESSAGE_TYPE(msg)) {
                LOG_I(TAG_PLAYER, "播放完成");
                is_eos = true;
            } else if (GST_MESSAGE_ERROR == GST_MESSAGE_TYPE(msg)) {
                GError *gerr = NULL;
                gchar *dbg = NULL;

                gst_message_parse_error(msg, &gerr, &dbg);
                LOG_E(TAG_PLAYER, "播放错误：%s",
                      (NULL != gerr) ? gerr->message : "未知错误");
                if (NULL != gerr) {
                    g_error_free(gerr);
                }
                if (NULL != dbg) {
                    g_free(dbg);
                }
                ret = -1;
            }
            gst_message_unref(msg);
            if (is_eos || (0 != ret)) {
                break;
            }
        }
        if (is_eos || (0 != ret)) {
            break;
        }

        /* 帧泵 + LVGL 刷新（按钮/触摸在此驱动） */
        pump_frame();
        {
            uint32_t wait_ms = lv_timer_handler();

            if (wait_ms > 10) {
                wait_ms = 10;
            }
            usleep(wait_ms * 1000);
        }
    }

    if (s_is_stop_requested) {
        ret = 1;
    }

    (void)gst_element_set_state(pipeline, GST_STATE_NULL);
    if (NULL != s_appsink) {
        gst_object_unref(s_appsink);
        s_appsink = NULL;
    }
    s_pipeline = NULL;
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return ret;
}

/*****************************************************************************
 * 函数名称：main
 * 功能描述：播放器入口——初始化 LVGL 后逐个播放指定文件
 * 输入参数：@argc - 参数个数（含程序名，至少 2）
 *           @argv - 参数列表，argv[1..] 为视频文件路径
 * 返回值：  0 全部播完，-1 参数错误或播放出错
 * 注意事项：LVGL/DRM/触摸全进程生命周期一次；播放完释放，主程序可收回屏幕
 *****************************************************************************/
int main(int argc, char **argv)
{
    int i = 0;
    int ret = 0;

    if (2 > argc) {
        fprintf(stderr, "用法：%s <视频文件> [视频文件 ...]\n", argv[0]);
        return -1;
    }

    (void)signal(SIGTERM, on_sigterm);
    (void)signal(SIGINT, on_sigterm);
    (void)gst_init(&argc, &argv);

    lv_init();
    drm_disp_drv_init(0, 0, 90); /* 旋转90°，与主程序一致 */
    (void)touch_input_init();
    ui_build();

    for (i = 1; i < argc; i++) {
        ret = play_file(argv[i]);
        if (1 == ret) {
            break; /* 播完/用户退出，不再播下一个 */
        }
    }

    /* 注意：不调用 drm_disp_drv_deinit()——本进程持有的 DRM fd
     * 关闭时会释放 DRM master，内核重置 CRTC，导致 UI 主程序
     * 黑屏；直接退出由进程清理，恢复画面由主程序侧完成 */
    LOG_I(TAG_PLAYER, "播放结束");
    return (0 == ret) ? 0 : -1;
}
