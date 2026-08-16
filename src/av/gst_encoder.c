/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：gst_encoder.c
 * 文件功能：GStreamer 分段录像编码器实现
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include <gst/gst.h>

#include "log.h"
#include <gst/app/gstappsink.h>

#include "gst_encoder.h"

/* 预览分支输出尺寸（tee 分流后缩放，供 AI 推理与 UI 显示，
 * 与 preview_share.h 的 PREVIEW_WIDTH/HEIGHT 保持一致） */
#define PREVIEW_BRANCH_W 1280
#define PREVIEW_BRANCH_H 720

/* 预览分支低延迟队列：只保留最新 1 帧，满时丢最旧（leaky=downstream） */
#define PREVIEW_QUEUE_BUFFERS 1
#define PREVIEW_QUEUE_LEAKY   2 /* GST_QUEUE_LEAK_DOWNSTREAM */

/* splitmuxsink 文件命名模板缓冲大小 */
#define LOCATION_BUF_SIZE 512

/* EOS 封口等待上限（秒），超时说明流水线卡死 */
#define EOS_WAIT_SEC 5

/*****************************************************************************
 * 函数名称：get_next_file_index
 * 功能描述：扫描目录找最大录像序号 +1，重启后新录像不会覆盖旧文件
 * 输入参数：@dir - 录像目录
 * 返回值：  下一个可用序号（目录不存在时返回 0）
 * 注意事项：只匹配 rec_%05d.mp4 或 rec_%05d_E.mp4，其他文件不参与序号
 *****************************************************************************/
static int get_next_file_index(const char *dir)
{
    DIR *d = NULL;
    struct dirent *entry = NULL;
    int max_index = -1;

    d = opendir(dir);
    if (NULL == d) {
        return 0;
    }

    while ((entry = readdir(d)) != NULL) {
        int idx;
        int consumed;

        /* %n 记录解析停止位置，剩余串必须是 .mp4 或 _E.mp4 才算录像文件 */
        if ((1 == sscanf(entry->d_name, "rec_%d%n", &idx, &consumed)) &&
            ((0 == strcmp(entry->d_name + consumed, ".mp4")) ||
             (0 == strcmp(entry->d_name + consumed, "_E.mp4")))) {
            if (idx > max_index) {
                max_index = idx;
            }
        }
    }
    closedir(d);

    return max_index + 1;
}


/*****************************************************************************
 * 函数名称：setup_sink_props
 * 功能描述：配置 splitmuxsink 属性（命名模板 / 分段时长 / 序号接续）
 * 输入参数：@sink   - splitmuxsink 元件
 *           @config - 编码配置
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：默认 mp4mux 在 EOS/切段时写 moov 索引。robust muxing
 *           方案已实测否决（1.24 下文件头刷新不写样本表，断电后
 *           文件不可播），断电保护依赖优雅退出与短分段兜底
 *****************************************************************************/
static int setup_sink_props(GstElement *sink,
                            const gst_encoder_config_t *config)
{
    char location[LOCATION_BUF_SIZE];
    int start_index;

    snprintf(location, sizeof(location), "%s/rec_%%05d.mp4", config->dir);
    start_index = get_next_file_index(config->dir);
    g_object_set(G_OBJECT(sink), "location", location, NULL);
    g_object_set(G_OBJECT(sink), "max-size-time",
                 (guint64)config->segment_sec * GST_SECOND, NULL);
    g_object_set(G_OBJECT(sink), "start-index", start_index, NULL);

    return 0;
}


/*****************************************************************************
 * 函数名称：appsink_new_sample
 * 功能描述：appsink 新帧回调——取出 NV12 数据转交预览回调（AI 推理）
 * 输入参数：@appsink   - 触发回调的 appsink
 *           @user_data - gst_encoder_t 上下文
 * 返回值：  GST_FLOW_OK / GST_FLOW_ERROR
 * 注意事项：在 GStreamer 内部线程执行，只做快速转发（回调内仅拷贝）
 *****************************************************************************/
static GstFlowReturn appsink_new_sample(GstAppSink *appsink,
                                        gpointer user_data)
{
    gst_encoder_t *enc = (gst_encoder_t *)user_data;
    GstSample *sample = NULL;
    GstBuffer *buffer = NULL;
    GstMapInfo map;

    sample = gst_app_sink_pull_sample(appsink);
    if (NULL == sample) {
        return GST_FLOW_ERROR;
    }

    buffer = gst_sample_get_buffer(sample);
    if ((NULL != buffer) && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (NULL != enc->preview_cb) {
            enc->preview_cb(map.data, enc->preview_user_data);
        }
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}


/*****************************************************************************
 * 函数名称：appsink_bgra_new_sample
 * 功能描述：BGRA appsink 新帧回调——转交 BGRA 预览回调（UI 显示）
 * 输入参数：@appsink   - 触发回调的 appsink
 *           @user_data - gst_encoder_t 上下文
 * 返回值：  GST_FLOW_OK / GST_FLOW_ERROR
 * 注意事项：BGRA 转换已在管线内 videoconvert 完成，UI 零转换开销
 *****************************************************************************/
static GstFlowReturn appsink_bgra_new_sample(GstAppSink *appsink,
                                             gpointer user_data)
{
    gst_encoder_t *enc = (gst_encoder_t *)user_data;
    GstSample *sample = NULL;
    GstBuffer *buffer = NULL;
    GstMapInfo map;

    sample = gst_app_sink_pull_sample(appsink);
    if (NULL == sample) {
        return GST_FLOW_ERROR;
    }

    buffer = gst_sample_get_buffer(sample);
    if ((NULL != buffer) && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (NULL != enc->preview_bgra_cb) {
            enc->preview_bgra_cb(map.data, enc->preview_bgra_user_data);
        }
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}


/*****************************************************************************
 * 函数名称：build_launch_string
 * 功能描述：构造 gst_parse_launch 管线描述串
 * 输入参数：@buf    - 输出缓冲
 *           @size   - 缓冲大小
 *           @config - 编码配置
 * 输出参数：@buf    - 管线描述串
 * 注意事项：全部元件带 name（后续按名取引用设置属性/回调）；
 *           音频分支按 audio_enabled 开关拼接
 *****************************************************************************/
static void build_launch_string(char *buf, size_t size,
                                const gst_encoder_config_t *config)
{
    int off;

    off = snprintf(buf, size,
        "v4l2src name=camsrc ! "
        "video/x-raw,format=NV12,width=%u,height=%u,framerate=%u/1 ! "
        "tee name=t "
        "t. ! queue ! mpph264enc bps=%u gop=%u ! "
        "valve name=rv drop=false ! h264parse ! "
        "splitmuxsink name=mux location=%s/rec_%%05d.mp4 "
        "max-size-time=%llu "
        "t. ! queue name=qp max-size-buffers=%d leaky=%d ! "
        "videoscale ! tee name=t2 "
        "t2. ! queue name=qn max-size-buffers=%d leaky=%d ! "
        "video/x-raw,format=NV12,width=%d,height=%d ! "
        "appsink name=preview_sink_nv12 sync=false drop=true "
        "max-buffers=1 "
        "t2. ! queue name=qb max-size-buffers=%d leaky=%d ! "
        "videoconvert ! video/x-raw,format=BGRA,width=%d,height=%d ! "
        "appsink name=preview_sink_bgra sync=false drop=true "
        "max-buffers=1 ",
        config->width, config->height, config->fps,
        config->bitrate, config->fps,
        config->dir,
        (unsigned long long)config->segment_sec * GST_SECOND,
        PREVIEW_QUEUE_BUFFERS, PREVIEW_QUEUE_LEAKY,
        PREVIEW_QUEUE_BUFFERS, PREVIEW_QUEUE_LEAKY,
        PREVIEW_BRANCH_W, PREVIEW_BRANCH_H,
        PREVIEW_QUEUE_BUFFERS, PREVIEW_QUEUE_LEAKY,
        PREVIEW_BRANCH_W, PREVIEW_BRANCH_H);

    /* 音频分支（启动期开关；手册官方配置：default 设备 +
     * 48k 单声道 + AAC，板载咪头物理单麦克风） */
    if (config->audio_enabled) {
        snprintf(buf + off, size - (size_t)off,
            "alsasrc device=default ! "
            "audio/x-raw,format=S16LE,rate=48000,channels=1 ! "
            "audioconvert ! audioresample ! avenc_aac bitrate=64000 ! "
            "aacparse ! mux.audio_0");
    }
}

/*****************************************************************************
 * 函数名称：create_record_pipeline
 * 功能描述：用 gst_parse_launch 创建并组装分段录像流水线
 * 输入参数：@enc    - 编码器上下文（保存 pipeline/bus/引用）
 *           @config - 编码配置
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：parse 构建与 gst-launch 完全同路径——手写 API
 *           构建在板端实测会致 mux 不输出（0 字节）且原因未
 *           定位，parse 构建稳定可靠
 *****************************************************************************/
static int create_record_pipeline(gst_encoder_t *enc,
                                  const gst_encoder_config_t *config)
{
    GstElement *pipeline = NULL;
    GstElement *elem = NULL;
    GstAppSinkCallbacks cb;
    char launch[2048];

    /* 录音增益（正点原子手册 2.4.5 官方配置：板载咪头默认
     * 增益很小，不设置录音音量不足） */
    if (config->audio_enabled) {
        system("/usr/bin/amixer -c 0 cset name='ADC OSR Volume ON' 'on'");
        system("/usr/bin/amixer -c 0 set 'ADC OSR' 100%");
        system("/usr/bin/amixer -c 0 set 'ADC2DAC Mixer' 90%");
    }

    build_launch_string(launch, sizeof(launch), config);
    pipeline = gst_parse_launch(launch, NULL);
    if (NULL == pipeline) {
        LOG_E("GST", "管线构建失败: %s", launch);
        return -1;
    }

    /* 采集设备 */
    elem = gst_bin_get_by_name(GST_BIN(pipeline), "camsrc");
    if (NULL == elem) {
        gst_object_unref(pipeline);
        return -1;
    }
    g_object_set(G_OBJECT(elem), "device", config->device, NULL);
    gst_object_unref(elem);

    /* splitmuxsink：序号接续 */
    elem = gst_bin_get_by_name(GST_BIN(pipeline), "mux");
    if (NULL == elem) {
        gst_object_unref(pipeline);
        return -1;
    }
    g_object_set(G_OBJECT(elem), "start-index",
                 get_next_file_index(config->dir), NULL);
    enc->record_sink = elem;
    gst_object_unref(elem);

    /* 录像阀门（主循环开关录像用） */
    enc->record_valve = gst_bin_get_by_name(GST_BIN(pipeline), "rv");

    /* 预览 appsink 双回调（AI 推理 / UI 显示） */
    memset(&cb, 0, sizeof(cb));
    cb.new_sample = appsink_new_sample;
    elem = gst_bin_get_by_name(GST_BIN(pipeline), "preview_sink_nv12");
    if (NULL != elem) {
        gst_app_sink_set_callbacks(GST_APP_SINK(elem), &cb, enc, NULL);
        gst_object_unref(elem);
    }
    memset(&cb, 0, sizeof(cb));
    cb.new_sample = appsink_bgra_new_sample;
    elem = gst_bin_get_by_name(GST_BIN(pipeline), "preview_sink_bgra");
    if (NULL != elem) {
        gst_app_sink_set_callbacks(GST_APP_SINK(elem), &cb, enc, NULL);
        gst_object_unref(elem);
    }

    enc->pipeline = pipeline;
    enc->bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    return 0;
}
/*****************************************************************************
 * 函数名称：gst_encoder_set_preview_cb
 * 功能描述：注册预览帧回调（tee 分支 appsink 新帧触发）
 * 输入参数：@enc       - 编码器上下文
 *           @cb        - 回调函数（NULL 取消）
 *           @user_data - 回调上下文
 * 注意事项：回调在 GStreamer 内部线程执行，必须快速返回
 *****************************************************************************/
void gst_encoder_set_preview_cb(gst_encoder_t *enc,
                                gst_preview_frame_cb cb, void *user_data)
{
    if (NULL == enc) {
        return;
    }

    enc->preview_cb = cb;
    enc->preview_user_data = user_data;
}


/*****************************************************************************
 * 函数名称：gst_encoder_set_preview_bgra_cb
 * 功能描述：注册 BGRA 预览帧回调（videoconvert 分支，UI 显示用）
 * 输入参数：@enc       - 编码器上下文
 *           @cb        - 回调函数（NULL 取消）
 *           @user_data - 回调上下文
 * 注意事项：BGRA 由管线内 videoconvert 完成，UI 侧零转换开销
 *****************************************************************************/
void gst_encoder_set_preview_bgra_cb(gst_encoder_t *enc,
                                     gst_preview_frame_cb cb,
                                     void *user_data)
{
    if (NULL == enc) {
        return;
    }

    enc->preview_bgra_cb = cb;
    enc->preview_bgra_user_data = user_data;
}


/*****************************************************************************
 * 函数名称：gst_encoder_init
 * 功能描述：创建分段录像流水线（参数校验 + 状态清理 + 组装）
 * 输入参数：@enc    - 编码器上下文
 *           @config - 编码配置
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：重复 init 会先清理旧流水线；调用契约 start→stop→deinit
 *****************************************************************************/
int gst_encoder_init(gst_encoder_t *enc, const gst_encoder_config_t *config)
{
    if ((NULL == enc) || (NULL == config) ||
        (NULL == config->device) || (NULL == config->dir)) {
        return -1;
    }

    /* 参数校验：0 无物理意义 */
    if ((0 == config->segment_sec) || (0 == config->width) ||
        (0 == config->height) || (0 == config->fps) ||
        (0 == config->bitrate)) {
        LOG_E("GST", "无效编码参数");
        return -1;
    }

    /* 重复 init 时先清理旧流水线，防止引用丢失 */
    if (NULL != enc->pipeline) {
        gst_encoder_deinit(enc);
    }
    memset(enc, 0, sizeof(*enc));

    /* GStreamer 框架初始化（幂等，可安全多次调用） */
    gst_init(NULL, NULL);

    return create_record_pipeline(enc, config);
}


/*****************************************************************************
 * 函数名称：gst_encoder_start
 * 功能描述：启动流水线（PLAYING），开始采集编码
 * 输入参数：@enc - 编码器上下文
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：须在 init 成功后调用
 *****************************************************************************/
int gst_encoder_start(gst_encoder_t *enc)
{
    GstStateChangeReturn ret;

    if ((NULL == enc) || (NULL == enc->pipeline)) {
        return -1;
    }

    /* NULL → PLAYING，GStreamer 内部自动经过 READY/PAUSED */
    ret = gst_element_set_state(enc->pipeline, GST_STATE_PLAYING);
    if (GST_STATE_CHANGE_FAILURE == ret) {
        LOG_E("GST", "流水线启动失败");
        return -1;
    }

    return 0;
}


/*****************************************************************************
 * 函数名称：gst_encoder_stop
 * 功能描述：停止流水线
 * 输入参数：@enc - 编码器上下文
 * 注意事项：必须发 EOS 让 splitmuxsink 把最后一个分段正确封口；
 *           EOS 2 秒未到达则告警（文件可能损坏）
 *****************************************************************************/
void gst_encoder_stop(gst_encoder_t *enc)
{
    GstMessage *msg;

    if ((NULL == enc) || (NULL == enc->pipeline)) {
        return;
    }

    /* 发 EOS：事件沿数据流向下游传播，splitmuxsink 收到后封口当前文件 */
    if (FALSE == gst_element_send_event(enc->pipeline,
                                        gst_event_new_eos())) {
        LOG_W("GST", "EOS 发送失败（流水线未运行？）");
    }

    /* 等 EOS 传递完（最多 EOS_WAIT_SEC 秒） */
    if (NULL != enc->bus) {
        msg = gst_bus_timed_pop_filtered(enc->bus,
                                         EOS_WAIT_SEC * GST_SECOND,
                                         GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        if (NULL == msg) {
            LOG_W("GST", "EOS 超时，最后分段可能不完整");
        } else {
            gst_message_unref(msg);
        }
    }

    /* 回到 NULL 状态 */
    gst_element_set_state(enc->pipeline, GST_STATE_NULL);
}


/*****************************************************************************
 * 函数名称：gst_encoder_set_record_enabled
 * 功能描述：录像开关（valve 数据闸门：关闭时预览继续、录像暂停，
 *           重开无缝续录当前分段）
 * 输入参数：@enc     - 编码器上下文
 *           @enabled - 是否录像
 *****************************************************************************/
void gst_encoder_set_record_enabled(gst_encoder_t *enc, bool enabled)
{
    if ((NULL == enc) || (NULL == enc->record_valve)) {
        return;
    }

    /* valve drop=true 时数据不再流向编码分支，splitmuxsink
     * 等待数据（当前段不封口），重开后无缝续录 */
    g_object_set(G_OBJECT(enc->record_valve), "drop", !enabled, NULL);
    LOG_I("GST", "录像%s", enabled ? "开启" : "暂停");
}


/*****************************************************************************
 * 函数名称：gst_encoder_set_segment_sec
 * 功能描述：运行时调整分段时长（splitmuxsink max-size-time）
 * 输入参数：@enc - 编码器上下文
 *           @sec - 分段时长（秒）
 *****************************************************************************/
void gst_encoder_set_segment_sec(gst_encoder_t *enc, uint32_t sec)
{
    if ((NULL == enc) || (NULL == enc->record_sink) || (0 == sec)) {
        return;
    }

    g_object_set(G_OBJECT(enc->record_sink), "max-size-time",
                 (guint64)sec * GST_SECOND, NULL);
    LOG_I("GST", "分段时长调整为 %u 秒", sec);
}


/*****************************************************************************
 * 函数名称：gst_encoder_deinit
 * 功能描述：销毁流水线，释放全部资源
 * 输入参数：@enc - 编码器上下文
 * 注意事项：防御性先回 NULL 态——未 stop 直接 deinit 时仍能触发
 *           EOS 收尾流程，避免当前分段缺 moov 损坏
 *****************************************************************************/
void gst_encoder_deinit(gst_encoder_t *enc)
{
    if (NULL == enc) {
        return;
    }

    if (NULL != enc->bus) {
        gst_object_unref(enc->bus);
        enc->bus = NULL;
    }

    if (NULL != enc->pipeline) {
        /* 幂等：已回 NULL 态时此调用无副作用 */
        gst_element_set_state(enc->pipeline, GST_STATE_NULL);
        gst_object_unref(enc->pipeline);
        enc->pipeline = NULL;
    }

    /* valve/sink 由 bin 析构释放，这里只清引用 */
    enc->record_valve = NULL;
    enc->record_sink  = NULL;
}
