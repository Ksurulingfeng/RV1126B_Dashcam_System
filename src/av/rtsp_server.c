/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：rtsp_server.c
 * 文件功能：RTSP 实时推流服务器实现
 *           主程序编码流经 appsrc 桥接进 gst-rtsp-server 的媒体管线，
 *           客户端 VLC/手机直接打开 rtsp://<ip>:8554/stream 实时观看
 * 作    者：heifast
 * 创建日期：2026-08-18
 *****************************************************************************/

#include <string.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>

#include "log.h"
#include "rtsp_server.h"

/* 模块日志标签 */
#define TAG_RTSP "RTSP"

/* RTSP 服务端口与挂载点（VLC 打开 rtsp://<ip>:8554/stream） */
#define RTSP_SERVICE "8554"
#define RTSP_MOUNT   "/stream"

/* 媒体管线：appsrc（外部编码流桥接）→ rtph264pay
 * 不做 h264parse：主程序侧 appsink 前已 h264parse 转 byte-stream；
 * config-interval=-1 每关键帧周期发 SPS/PPS——实时接入的客户端
 * 中途连接，靠周期参数集才能解码（默认 0 会黑屏）；
 * do-timestamp=true 关键：外部编码流 PTS 是主程序管线时间（可能已
 * 运行很久），而 RTSP 媒体管线连接时才创建时钟从 0——用到达时刻
 * 生成时间戳，避免 appsrc 等"未来"running time 导致 RTP 卡死 */
#define RTSP_MEDIA_LAUNCH \
    "appsrc name=src format=time ! queue ! " \
    "rtph264pay name=pay0 pt=96 config-interval=-1"

/* 服务器状态 */
static GstRTSPServer *s_server = NULL; /* RTSP 服务器 */
static GMainLoop *s_loop = NULL;       /* GLib 主循环 */
static GThread *s_thread = NULL;       /* 服务器线程 */
static GstAppSrc *s_appsrc = NULL;     /* 当前活动客户端的编码源 */
static GMutex s_mutex;                 /* appsrc 互斥 */

/* 首帧 PTS 基线：外部编码流 PTS 从主程序启动起算（可能巨大），
 * RTSP 媒体管线时钟从 0 开始，需重写为相对时间戳才匹配 */
static GstClockTime s_rtsp_base_pts = GST_CLOCK_TIME_NONE;

/*****************************************************************************
 * 函数名称：media_configure
 * 功能描述：RTSP 媒体配置回调——取出媒体管线的 appsrc 保存，
 *           作为外部编码流的推入目标（单客户端：新连接覆盖旧引用）
 * 输入参数：@factory    - 媒体工厂
 *           @media      - 媒体实例（含管线）
 *           @user_data  - 未使用
 *****************************************************************************/
static void media_configure(GstRTSPMediaFactory *factory,
                            GstRTSPMedia *media, gpointer user_data)
{
    GstElement *elem = NULL;

    (void)factory;
    (void)user_data;

    elem = gst_bin_get_by_name(GST_BIN(gst_rtsp_media_get_element(media)),
                               "src");
    if (NULL == elem) {
        LOG_E(TAG_RTSP, "媒体管线缺少 appsrc");
        return;
    }

    g_mutex_lock(&s_mutex);
    if (NULL != s_appsrc) {
        g_object_unref(s_appsrc);
    }
    s_appsrc = GST_APP_SRC(elem);
    s_rtsp_base_pts = GST_CLOCK_TIME_NONE; /* 新会话重置时间戳基线 */
    g_mutex_unlock(&s_mutex);
    LOG_I(TAG_RTSP, "客户端连接，开始推流");
}

/*****************************************************************************
 * 函数名称：server_thread
 * 功能描述：RTSP 服务器线程——跑 GLib 主循环（处理客户端会话）
 * 输入参数：@data - 未使用
 * 返回值：  始终 NULL
 *****************************************************************************/
static gpointer server_thread(gpointer data)
{
    (void)data;
    g_main_loop_run(s_loop);
    return NULL;
}

/*****************************************************************************
 * 函数名称：rtsp_server_init
 * 功能描述：创建并启动 RTSP 服务器（8554 端口 /stream 挂载点）
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：服务器线程常驻；媒体管线为 appsrc 桥接，
 *           编码流由外部 rtsp_server_push 注入
 *****************************************************************************/
int rtsp_server_init(void)
{
    GstRTSPMountPoints *mounts = NULL;
    GstRTSPMediaFactory *factory = NULL;

    s_loop = g_main_loop_new(NULL, FALSE);
    s_server = gst_rtsp_server_new();
    if (NULL == s_server) {
        LOG_E(TAG_RTSP, "RTSP 服务器创建失败");
        return -1;
    }
    g_object_set(s_server, "service", RTSP_SERVICE, NULL);

    mounts = gst_rtsp_server_get_mount_points(s_server);
    factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(factory, RTSP_MEDIA_LAUNCH);
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect(factory, "media-configure",
                     G_CALLBACK(media_configure), NULL);
    gst_rtsp_mount_points_add_factory(mounts, RTSP_MOUNT, factory);
    g_object_unref(mounts);

    (void)gst_rtsp_server_attach(s_server, NULL);
    s_thread = g_thread_new("rtsp-server", server_thread, NULL);
    LOG_I(TAG_RTSP, "RTSP 服务器已启动：rtsp://<ip>:%s%s",
          RTSP_SERVICE, RTSP_MOUNT);
    return 0;
}

/*****************************************************************************
 * 函数名称：rtsp_server_push
 * 功能描述：把一帧编码流 buffer 推给当前 RTSP 客户端
 * 输入参数：@buf  - 编码流 buffer（内部 +1 引用）
 *           @caps - 编码流 caps（与 appsrc 当前 caps 不同才更新）
 * 注意事项：无客户端（s_appsrc 空）时直接丢弃；多客户端时只服务
 *           最新连接的（单客户端简化）
 *****************************************************************************/
void rtsp_server_push(GstBuffer *buf, GstCaps *caps)
{
    if ((NULL == buf) || (NULL == caps)) {
        return;
    }

    g_mutex_lock(&s_mutex);
    if (NULL != s_appsrc) {
        GstCaps *old_caps = gst_app_src_get_caps(s_appsrc);
        GstBuffer *out = NULL;
        GstFlowReturn fr;
        GstClockTime pts;

        /* 重写时间戳为相对基线：外部编码流 PTS 从主程序启动起算
         * 可能巨大，RTSP 媒体管线时钟从 0，不重写会导致 RTP 时间戳
         * 异常（rtpbin 不发包） */
        pts = GST_BUFFER_PTS(buf);
        if (GST_CLOCK_TIME_NONE == s_rtsp_base_pts) {
            s_rtsp_base_pts = pts;
        }
        out = gst_buffer_copy(buf);
        GST_BUFFER_PTS(out) = pts - s_rtsp_base_pts;
        GST_BUFFER_DTS(out) = GST_BUFFER_PTS(out);

        if ((NULL == old_caps) || (!gst_caps_is_equal(old_caps, caps))) {
            gst_app_src_set_caps(s_appsrc, caps);
        }
        if (NULL != old_caps) {
            gst_caps_unref(old_caps);
        }
        fr = gst_app_src_push_buffer(s_appsrc, out);
        if (GST_FLOW_OK != fr) {
            LOG_W(TAG_RTSP, "appsrc push 失败 %d", fr);
            gst_buffer_unref(out);
        }
    }
    g_mutex_unlock(&s_mutex);
}

/*****************************************************************************
 * 函数名称：rtsp_server_deinit
 * 功能描述：停止 RTSP 服务器线程并释放全部资源
 * 注意事项：主程序退出时调用；须在 gst_encoder_deinit 之后
 *****************************************************************************/
void rtsp_server_deinit(void)
{
    g_mutex_lock(&s_mutex);
    if (NULL != s_appsrc) {
        g_object_unref(s_appsrc);
        s_appsrc = NULL;
    }
    g_mutex_unlock(&s_mutex);

    if (NULL != s_loop) {
        g_main_loop_quit(s_loop);
    }
    if (NULL != s_thread) {
        (void)g_thread_join(s_thread);
        s_thread = NULL;
    }
    if (NULL != s_server) {
        g_object_unref(s_server);
        s_server = NULL;
    }
    if (NULL != s_loop) {
        g_main_loop_unref(s_loop);
        s_loop = NULL;
    }
    LOG_I(TAG_RTSP, "RTSP 服务器已停止");
}
