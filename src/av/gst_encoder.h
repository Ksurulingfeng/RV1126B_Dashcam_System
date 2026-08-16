/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：gst_encoder.h
 * 文件功能：GStreamer 录像编码器
 *           v4l2src → capsfilter → mpph264enc → h264parse → splitmuxsink
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#ifndef GST_ENCODER_H
#define GST_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#include <gst/gst.h>

/* 编码器配置（打包参数，避免超长参数列表） */
typedef struct {
    const char *device;       /* 摄像头设备节点，如 "/dev/video-camera0" */
    const char *dir;          /* 录像输出目录 */
    uint32_t    segment_sec;  /* 单文件时长（秒），如 300 = 5 分钟 */
    uint32_t    width;        /* 编码宽度 */
    uint32_t    height;       /* 编码高度 */
    uint32_t    fps;          /* 帧率 */
    uint32_t    bitrate;      /* 码率（bps），如 8000000 */
} gst_encoder_config_t;

/* 预览帧回调：appsink 收到新帧时调用（GStreamer 内部线程）
 * @data      - 帧数据（格式由注册接口决定）
 * @user_data - 注册时传入的上下文
 * 注意事项：回调在 GStreamer 内部线程执行，必须快速返回（仅拷贝） */
typedef void (*gst_preview_frame_cb)(const uint8_t *data, void *user_data);

/* 录像编码器上下文 */
typedef struct {
    GstElement *pipeline;      /* 编码流水线 */
    GstBus     *bus;           /* 消息总线（错误/EOS 监控） */
    GstElement *record_valve;  /* 录像阀门（tee→valve→编码，开关录像用） */
    GstElement *record_sink;   /* splitmuxsink（运行时调分段时长） */
    gst_preview_frame_cb preview_cb;       /* NV12 预览帧回调（AI 推理） */
    void *preview_user_data;               /* NV12 回调上下文 */
    gst_preview_frame_cb preview_bgra_cb;  /* BGRA 预览帧回调（UI 显示） */
    void *preview_bgra_user_data;          /* BGRA 回调上下文 */
} gst_encoder_t;

/*****************************************************************************
 * 函数名称：gst_encoder_init
 * 功能描述：创建分段录像流水线
 * 输入参数：@enc    - 编码器上下文
 *           @config - 编码配置（见 gst_encoder_config_t）
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：文件自动命名为 dir/rec_%05d.mp4，序号在已有文件基础上
 *           递增，重启不会覆盖旧录像
 *****************************************************************************/
int gst_encoder_init(gst_encoder_t *enc, const gst_encoder_config_t *config);

/*****************************************************************************
 * 函数名称：gst_encoder_start
 * 功能描述：启动流水线，开始采集编码写文件
 * 输入参数：@enc - 编码器上下文
 * 返回值：  成功返回0，失败返回-1
 *****************************************************************************/
int gst_encoder_start(gst_encoder_t *enc);

/*****************************************************************************
 * 函数名称：gst_encoder_stop
 * 功能描述：停止流水线（发 EOS 让最后一个分段正确收尾写索引）
 * 输入参数：@enc - 编码器上下文
 *****************************************************************************/
void gst_encoder_stop(gst_encoder_t *enc);

/*****************************************************************************
 * 函数名称：gst_encoder_set_preview_cb
 * 功能描述：注册 NV12 预览帧回调（tee 分支 appsink 新帧触发，AI 推理用）
 * 输入参数：@enc       - 编码器上下文
 *           @cb        - 回调函数（NULL 取消）
 *           @user_data - 回调上下文
 * 注意事项：须在 gst_encoder_init 之后、start 之前调用；
 *           回调在 GStreamer 内部线程执行，必须快速返回
 *****************************************************************************/
void gst_encoder_set_preview_cb(gst_encoder_t *enc,
                                gst_preview_frame_cb cb, void *user_data);

/*****************************************************************************
 * 函数名称：gst_encoder_set_preview_bgra_cb
 * 功能描述：注册 BGRA 预览帧回调（videoconvert 分支，UI 显示用）
 * 输入参数：@enc       - 编码器上下文
 *           @cb        - 回调函数（NULL 取消）
 *           @user_data - 回调上下文
 * 注意事项：BGRA 由 GStreamer 管线内 videoconvert 完成，
 *           UI 侧零转换开销；回调同样必须快速返回
 *****************************************************************************/
void gst_encoder_set_preview_bgra_cb(gst_encoder_t *enc,
                                     gst_preview_frame_cb cb,
                                     void *user_data);

/*****************************************************************************
 * 函数名称：gst_encoder_deinit
 * 功能描述：销毁流水线，释放全部资源
 * 输入参数：@enc - 编码器上下文
 *****************************************************************************/
void gst_encoder_deinit(gst_encoder_t *enc);

/*****************************************************************************
 * 函数名称：gst_encoder_set_record_enabled
 * 功能描述：录像开关（valve 数据闸门：关闭时预览继续、录像暂停，
 *           重开无缝续录当前分段）
 * 输入参数：@enc     - 编码器上下文
 *           @enabled - 是否录像
 *****************************************************************************/
void gst_encoder_set_record_enabled(gst_encoder_t *enc, bool enabled);

/*****************************************************************************
 * 函数名称：gst_encoder_set_segment_sec
 * 功能描述：运行时调整分段时长（splitmuxsink max-size-time）
 * 输入参数：@enc - 编码器上下文
 *           @sec - 分段时长（秒）
 *****************************************************************************/
void gst_encoder_set_segment_sec(gst_encoder_t *enc, uint32_t sec);

#endif /* GST_ENCODER_H */
