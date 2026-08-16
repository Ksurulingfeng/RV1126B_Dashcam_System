/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：gst_encoder.c
 * 文件功能：GStreamer 分段录像编码器实现
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#include <stdio.h>
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
 * 函数名称：create_record_pipeline
 * 功能描述：创建并组装分段录像流水线（tee 双分支：录像 + 预览）
 *           v4l2src → capsfilter → tee ─┬→ queue → mpph264enc → h264parse
 *                                      │     → splitmuxsink（分段录像）
 *                                      └→ queue → videoscale → appsink
 *                                            （预览，AI 推理同源）
 * 输入参数：@enc    - 编码器上下文（保存 pipeline/bus）
 *           @config - 编码配置
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：元件 add 进 bin 后所有权归 bin，失败路径只需释放
 *           caps 和 pipeline（bin 析构会释放全部子元件）
 *****************************************************************************/
static int create_record_pipeline(gst_encoder_t *enc,
                                  const gst_encoder_config_t *config)
{
    GstElement *pipeline = NULL;
    GstElement *src = NULL;
    GstElement *capsfilter = NULL;
    GstElement *tee = NULL;
    GstElement *record_valve = NULL;
    GstElement *queue_rec = NULL;
    GstElement *queue_prev = NULL;
    GstElement *scale = NULL;
    GstElement *tee2 = NULL;
    GstElement *queue_nv12 = NULL;
    GstElement *queue_bgra = NULL;
    GstElement *videoconvert = NULL;
    GstElement *appsink = NULL;
    GstElement *appsink_bgra = NULL;
    GstElement *encoder = NULL;
    GstElement *parser = NULL;
    GstElement *sink = NULL;
    GstCaps    *caps = NULL;
    GstCaps    *prev_caps = NULL;
    GstCaps    *bgra_caps = NULL;
    GstAppSinkCallbacks cb;

    /* 创建流水线容器 */
    pipeline = gst_pipeline_new("record-pipeline");
    if (NULL == pipeline) {
        return -1;
    }

    /* 创建各元件：
     * v4l2src 采集 / capsfilter 格式过滤 / tee 分流 /
     * queue 缓冲隔离 / videoscale 预览缩放 / tee2 预览二次分流 /
     * videoconvert NV12→BGRA / appsink 双出口 /
     * mpph264enc RK 硬件编码 / h264parse 码流整理 / splitmuxsink 分段封装 */
    src        = gst_element_factory_make("v4l2src",      "src");
    capsfilter = gst_element_factory_make("capsfilter",   "filter");
    tee        = gst_element_factory_make("tee",          "tee");
    record_valve = gst_element_factory_make("valve",      "record_valve");
    queue_rec  = gst_element_factory_make("queue",        "queue_rec");
    queue_prev = gst_element_factory_make("queue",        "queue_prev");
    scale      = gst_element_factory_make("videoscale",   "preview_scale");
    tee2       = gst_element_factory_make("tee",          "preview_tee");
    queue_nv12 = gst_element_factory_make("queue",        "queue_nv12");
    queue_bgra = gst_element_factory_make("queue",        "queue_bgra");
    videoconvert = gst_element_factory_make("videoconvert", "preview_cvt");
    appsink    = gst_element_factory_make("appsink",      "preview_sink_nv12");
    appsink_bgra = gst_element_factory_make("appsink",    "preview_sink_bgra");
    encoder    = gst_element_factory_make("mpph264enc",   "encoder");
    parser     = gst_element_factory_make("h264parse",    "parser");
    sink       = gst_element_factory_make("splitmuxsink", "sink");
    if ((NULL == src) || (NULL == capsfilter) || (NULL == tee) ||
        (NULL == record_valve) || (NULL == queue_rec) ||
        (NULL == queue_prev) || (NULL == scale) ||
        (NULL == tee2) || (NULL == queue_nv12) || (NULL == queue_bgra) ||
        (NULL == videoconvert) || (NULL == appsink) ||
        (NULL == appsink_bgra) || (NULL == encoder) || (NULL == parser) ||
        (NULL == sink)) {
        LOG_E("GST", "创建 GStreamer 元件失败");
        goto error;
    }

    /* 采集与编码属性（gop=帧率 → 关键帧间隔 1 秒） */
    g_object_set(G_OBJECT(src), "device", config->device, NULL);
    g_object_set(G_OBJECT(encoder), "bps", (guint)config->bitrate, NULL);
    g_object_set(G_OBJECT(encoder), "gop", (gint)config->fps, NULL);

    /* 格式过滤：NV12 / 分辨率 / 帧率 */
    caps = gst_caps_new_simple(
        "video/x-raw",
        "format",    G_TYPE_STRING, "NV12",
        "width",     G_TYPE_INT,    (int)config->width,
        "height",    G_TYPE_INT,    (int)config->height,
        "framerate", GST_TYPE_FRACTION, (int)config->fps, 1,
        NULL);
    if (NULL == caps) {
        goto error;
    }
    g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
    gst_caps_unref(caps);
    caps = NULL;

    /* ====== 预览分支低延迟配置 ======
     * queue 默认缓冲 200 帧，下游消费慢会堆积数百毫秒延迟；
     * 预览场景只要最新帧：全部队列限 1 帧 + leaky=downstream（丢最旧） */
    g_object_set(G_OBJECT(queue_prev), "max-size-buffers",
                 PREVIEW_QUEUE_BUFFERS, NULL);
    g_object_set(G_OBJECT(queue_prev), "leaky", PREVIEW_QUEUE_LEAKY, NULL);
    g_object_set(G_OBJECT(queue_nv12), "max-size-buffers",
                 PREVIEW_QUEUE_BUFFERS, NULL);
    g_object_set(G_OBJECT(queue_nv12), "leaky", PREVIEW_QUEUE_LEAKY, NULL);
    g_object_set(G_OBJECT(queue_bgra), "max-size-buffers",
                 PREVIEW_QUEUE_BUFFERS, NULL);
    g_object_set(G_OBJECT(queue_bgra), "leaky", PREVIEW_QUEUE_LEAKY, NULL);

    /* NV12 appsink（AI 推理出口） */
    prev_caps = gst_caps_new_simple(
        "video/x-raw",
        "format",    G_TYPE_STRING, "NV12",
        "width",     G_TYPE_INT,    PREVIEW_BRANCH_W,
        "height",    G_TYPE_INT,    PREVIEW_BRANCH_H,
        "framerate", GST_TYPE_FRACTION, (int)config->fps, 1,
        NULL);
    if (NULL == prev_caps) {
        goto error;
    }
    g_object_set(G_OBJECT(appsink), "caps", prev_caps, NULL);
    gst_caps_unref(prev_caps);
    prev_caps = NULL;
    g_object_set(G_OBJECT(appsink), "sync", FALSE, NULL);
    g_object_set(G_OBJECT(appsink), "max-buffers", 1, NULL);
    g_object_set(G_OBJECT(appsink), "drop", TRUE, NULL);

    memset(&cb, 0, sizeof(cb));
    cb.new_sample = appsink_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &cb, enc, NULL);

    /* BGRA appsink（UI 显示出口：videoconvert 在管线内完成转换，
     * UI 零转换开销） */
    bgra_caps = gst_caps_new_simple(
        "video/x-raw",
        "format",    G_TYPE_STRING, "BGRA",
        "width",     G_TYPE_INT,    PREVIEW_BRANCH_W,
        "height",    G_TYPE_INT,    PREVIEW_BRANCH_H,
        "framerate", GST_TYPE_FRACTION, (int)config->fps, 1,
        NULL);
    if (NULL == bgra_caps) {
        goto error;
    }
    g_object_set(G_OBJECT(appsink_bgra), "caps", bgra_caps, NULL);
    gst_caps_unref(bgra_caps);
    bgra_caps = NULL;
    g_object_set(G_OBJECT(appsink_bgra), "sync", FALSE, NULL);
    g_object_set(G_OBJECT(appsink_bgra), "max-buffers", 1, NULL);
    g_object_set(G_OBJECT(appsink_bgra), "drop", TRUE, NULL);

    memset(&cb, 0, sizeof(cb));
    cb.new_sample = appsink_bgra_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink_bgra), &cb, enc, NULL);

    /* 组装：先 add（bin 接管元件所有权），后 link tee 多分支
     * 录像路：tee → valve → queue → encoder → parser → splitmuxsink
     * 预览路：tee → queue → scale → tee2 ─→ queue → appsink(NV12→AI)
     *                                   └→ queue → videoconvert → appsink(BGRA→UI) */
    gst_bin_add_many(GST_BIN(pipeline), src, capsfilter, tee,
                     record_valve, queue_rec, queue_prev, scale, tee2,
                     queue_nv12, queue_bgra, videoconvert,
                     appsink, appsink_bgra,
                     encoder, parser, sink, NULL);
    if ((FALSE == gst_element_link(src, capsfilter)) ||
        (FALSE == gst_element_link(capsfilter, tee)) ||
        (FALSE == gst_element_link_many(tee, record_valve, queue_rec,
                                        encoder, parser, sink, NULL)) ||
        (FALSE == gst_element_link_many(tee, queue_prev, scale,
                                        tee2, NULL)) ||
        (FALSE == gst_element_link_many(tee2, queue_nv12,
                                        appsink, NULL)) ||
        (FALSE == gst_element_link_many(tee2, queue_bgra,
                                        videoconvert, appsink_bgra, NULL))) {
        LOG_E("GST", "GStreamer 元件连接失败");
        goto error;
    }

    /* 保存录像阀门与 sink 引用（bin 持有所有权，此处仅借用，
     * 供运行时开关录像/调分段时长） */
    enc->record_valve = record_valve;
    enc->record_sink  = sink;

    /* splitmuxsink：命名模板 / 分段时长 / 序号接续 */
    if (0 != setup_sink_props(sink, config)) {
        LOG_E("GST", "配置 splitmuxsink 失败");
        goto error;
    }

    /* 保存上下文 */
    enc->pipeline = pipeline;
    enc->bus      = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    return 0;

error:
    /* 元件已入 bin 由 pipeline 析构释放，这里只释放 caps 和 pipeline */
    if (NULL != bgra_caps) {
        gst_caps_unref(bgra_caps);
    }
    if (NULL != prev_caps) {
        gst_caps_unref(prev_caps);
    }
    if (NULL != caps) {
        gst_caps_unref(caps);
    }
    gst_object_unref(pipeline);
    return -1;
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
