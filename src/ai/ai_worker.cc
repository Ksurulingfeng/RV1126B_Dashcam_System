/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：ai_worker.cc
 * 文件功能：AI 目标检测线程实现 —— OpenCV 取帧 + RKNN 推理 + 后处理
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include <opencv2/opencv.hpp>

#include "rknn_api.h"

#include "ai_worker.h"
#include "postprocess.h"
#include "preprocess.h"

#include "log.h"

/* 日志打印间隔（帧），避免每帧 printf 刷屏 */
#define AI_LOG_INTERVAL 30

/* YOLOv5 三输出头，其他结构模型不支持 */
#define AI_EXPECT_OUTPUTS 3

/* person 连续检测帧数阈值：达到后锁定最新录像文件（紧急录像联动） */
#define AI_PERSON_LOCK_STREAK 3

/* 连续无 person 帧数阈值：复位锁定状态，允许下一轮事件触发
 * 防检测抖动（person 边缘帧时有时无）导致同段录像反复锁定 */
#define AI_PERSON_CLEAR_STREAK 15

/* 预览帧 NV12 缓冲（GStreamer appsink 输出，与录像同源） */
static uint8_t s_nv12_buf[PREVIEW_SIZE];

/* 无新帧时的轮询间隔（微秒）：30fps 帧间隔 33ms，
 * 5ms 轮询在响应速度与 CPU 占用间取平衡，防止忙等烧核 */
#define AI_IDLE_WAIT_US 5000

/* RKNN 模型信息（输入尺寸与 IO 属性打包，精简参数列表） */
typedef struct {
    rknn_input_output_num io_num;
    rknn_tensor_attr in_attr;
    int model_w;
    int model_h;
    int model_c;
} rknn_model_info_t;

/* 推理循环上下文（打包循环状态，便于拆分长函数） */
typedef struct {
    ai_worker_t* worker;
    rknn_context ctx;
    const rknn_model_info_t* info;
    const rknn_tensor_attr* out_attrs;
    uint32_t frame_index;
    uint32_t last_frame_id; /* 本消费者已读的预览帧序号 */
    int person_streak;
    int no_person_streak;
    bool is_person_locked;
    char locked_path[FILE_PATH_MAX];
} ai_loop_ctx_t;

/* letterbox 预处理结果（pads 与 scale 成对传递） */
typedef struct {
    BOX_RECT pads;
    float scale;
} preproc_result_t;

/*****************************************************************************
 * 函数名称：get_time_ms
 * 功能描述：获取当前时间（毫秒，gettimeofday 实现）
 * 返回值：  毫秒级时间戳（double）
 *****************************************************************************/
static double get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000000.0 + tv.tv_usec) / 1000.0;
}

/*****************************************************************************
 * 函数名称：result_has_person
 * 功能描述：判断检测结果中是否包含 person 类别
 * 输入参数：@group - 检测结果组
 * 返回值：  含 person 返回 true，否则 false
 *****************************************************************************/
static bool result_has_person(const detect_result_group_t* group)
{
    for (int i = 0; i < group->count; i++) {
        if (0 == strcmp(group->results[i].name, "person")) {
            return true;
        }
    }
    return false;
}

/*****************************************************************************
 * 函数名称：loop_push_detect
 * 功能描述：把检测结果转成 C 结构推给 UI（画框在 UI 层完成）
 * 输入参数：@lc           - 推理循环上下文
 *           @result_group - 本帧检测结果
 * 注意事项：与预览帧通过 frame_id 关联，UI 按帧号对齐画框
 *****************************************************************************/
static void loop_push_detect(ai_loop_ctx_t* lc,
                             const detect_result_group_t* result_group)
{
    detect_box_t boxes[DETECT_BOX_MAX];
    uint32_t n;
    uint32_t i;

    if (NULL == lc->worker->detect_share) {
        return;
    }

    n = (uint32_t)result_group->count;
    if (n > DETECT_BOX_MAX) {
        n = DETECT_BOX_MAX;
    }

    for (i = 0; i < n; i++) {
        const detect_result_t* r = &result_group->results[i];
        boxes[i].left            = r->box.left;
        boxes[i].top             = r->box.top;
        boxes[i].right           = r->box.right;
        boxes[i].bottom          = r->box.bottom;
        strncpy(boxes[i].name, r->name, DETECT_NAME_MAX - 1);
        boxes[i].name[DETECT_NAME_MAX - 1] = '\0';
    }

    if (0 < n) {
        detect_share_push(lc->worker->detect_share, boxes, n,
                          lc->last_frame_id);
    }
}

/*****************************************************************************
 * 函数名称：load_model
 * 功能描述：读取 .rknn 模型文件到内存
 * 输入参数：@path       - 模型文件路径
 * 输出参数：@model_data - 模型数据缓冲
 *           @model_size - 模型大小
 * 返回值：  成功0，失败-1
 *****************************************************************************/
static int load_model(const char* path, unsigned char** model_data,
                      int* model_size)
{
    FILE* fp = NULL;
    long size;

    fp = fopen(path, "rb");
    if (NULL == fp) {
        LOG_E("AI", "打开模型失败: %s", path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    *model_data = (unsigned char*)malloc((size_t)size);
    if (NULL == *model_data) {
        fclose(fp);
        return -1;
    }

    if (size != (long)fread(*model_data, 1, (size_t)size, fp)) {
        LOG_E("AI", "读取模型数据失败");
        fclose(fp);
        free(*model_data);
        *model_data = NULL;
        return -1;
    }

    fclose(fp);
    *model_size = (int)size;
    return 0;
}

/*****************************************************************************
 * 函数名称：init_rknn
 * 功能描述：初始化 RKNN 上下文，查询模型输入输出属性
 * 输入参数：@ctx       - RKNN 上下文（输出）
 *           @model_data/model_size - 模型数据
 * 输出参数：@info      - 模型信息（IO 属性 + 输入尺寸）
 *           @out_attrs - 输出属性数组（调用者释放）
 * 返回值：  成功0，失败-1
 *****************************************************************************/
static int init_rknn(rknn_context* ctx, unsigned char* model_data,
                     int model_size, rknn_model_info_t* info,
                     rknn_tensor_attr** out_attrs)
{
    int ret;

    ret = rknn_init(ctx, model_data, model_size, 0, NULL);
    if (ret < 0) {
        LOG_E("AI", "rknn_init 失败 ret=%d", ret);
        return -1;
    }

    memset(&info->io_num, 0, sizeof(info->io_num));
    ret = rknn_query(*ctx, RKNN_QUERY_IN_OUT_NUM, &info->io_num,
                     sizeof(info->io_num));
    if (ret < 0) {
        LOG_E("AI", "查询输入输出数量失败");
        rknn_destroy(*ctx);
        return -1;
    }

    /* 本实现仅支持 YOLOv5 三输出结构 */
    if (AI_EXPECT_OUTPUTS != info->io_num.n_output) {
        LOG_E("AI", "模型输出数 %d 不支持（需 %d）",
                info->io_num.n_output, AI_EXPECT_OUTPUTS);
        rknn_destroy(*ctx);
        return -1;
    }

    memset(&info->in_attr, 0, sizeof(info->in_attr));
    info->in_attr.index = 0;
    ret                 = rknn_query(*ctx, RKNN_QUERY_INPUT_ATTR, &info->in_attr,
                                     sizeof(rknn_tensor_attr));
    if (ret < 0) {
        LOG_E("AI", "查询输入属性失败");
        rknn_destroy(*ctx);
        return -1;
    }

    *out_attrs = (rknn_tensor_attr*)malloc(sizeof(rknn_tensor_attr) *
                                           (size_t)info->io_num.n_output);
    if (NULL == *out_attrs) {
        rknn_destroy(*ctx);
        return -1;
    }

    for (int i = 0; i < info->io_num.n_output; i++) {
        memset(&(*out_attrs)[i], 0, sizeof(rknn_tensor_attr));
        (*out_attrs)[i].index = i;
        ret                   = rknn_query(*ctx, RKNN_QUERY_OUTPUT_ATTR, &(*out_attrs)[i],
                                           sizeof(rknn_tensor_attr));
        if (ret < 0) {
            LOG_E("AI", "查询输出属性失败");
            free(*out_attrs);
            *out_attrs = NULL;
            rknn_destroy(*ctx);
            return -1;
        }
    }

    /* NHWC: [1, H, W, C] */
    info->model_h = info->in_attr.dims[1];
    info->model_w = info->in_attr.dims[2];
    info->model_c = info->in_attr.dims[3];

    return 0;
}

/*****************************************************************************
 * 函数名称：loop_capture_show
 * 功能描述：从预览共享取帧 → NV12 转 BGR（推理输入，不再推帧给 UI）
 * 输入参数：@lc    - 推理循环上下文
 * 输出参数：@frame - 本帧 BGR 画面（直接作为推理输入）
 * 返回值：  有新帧处理返回 true，无新帧 false
 * 注意事项：帧源为 GStreamer tee 分支（appsink），与录像同源同视野；
 *           画框与显示已移至 UI 层，AI 无需保护副本
 *****************************************************************************/
static bool loop_capture_show(ai_loop_ctx_t* lc, cv::Mat& frame)
{
    cv::Mat yuv;

    if (!preview_share_pop(lc->worker->preview_share, s_nv12_buf,
                           &lc->last_frame_id)) {
        return false;
    }
    lc->frame_index++;

    /* NV12 → BGR（appsink 输出标准 NV12，OpenCV 直接转换） */
    yuv = cv::Mat(PREVIEW_HEIGHT * 3 / 2, PREVIEW_WIDTH, CV_8UC1,
                  s_nv12_buf);
    cv::cvtColor(yuv, frame, cv::COLOR_YUV2BGR_NV12);

    return true;
}

/*****************************************************************************
 * 函数名称：loop_preprocess
 * 功能描述：letterbox 缩放 RGB 帧到模型输入尺寸
 * 输入参数：@lc   - 推理循环上下文
 *           @frame  - BGR 帧（推理输入）
 *           @resized - 输出：模型输入图（已分配）
 * 返回值：  letterbox 结果（pads 与 scale，供坐标反变换）
 *****************************************************************************/
static preproc_result_t loop_preprocess(ai_loop_ctx_t* lc, const cv::Mat& frame,
                                        cv::Mat& resized)
{
    preproc_result_t result;
    int model_w = lc->info->model_w;
    int model_h = lc->info->model_h;

    memset(&result, 0, sizeof(result));
    result.scale = (float)model_w / frame.cols;
    if ((float)model_h / frame.rows < result.scale) {
        result.scale = (float)model_h / frame.rows;
    }
    letterbox(frame, resized, result.pads, result.scale,
              cv::Size(model_w, model_h));

    return result;
}

/*****************************************************************************
 * 函数名称：loop_run_inference
 * 功能描述：NPU 推理 + NMS 后处理 + 降频打印
 * 输入参数：@lc      - 推理循环上下文
 *           @resized - 模型输入图
 *           @preproc - letterbox 参数（坐标反变换用）
 * 输出参数：@result_group - 检测结果
 *           @infer_ms     - 推理耗时（毫秒）
 * 返回值：  成功返回 true，RKNN 调用失败 false
 *****************************************************************************/
static bool loop_run_inference(ai_loop_ctx_t* lc, cv::Mat& resized,
                               const preproc_result_t* preproc,
                               detect_result_group_t* result_group,
                               double* infer_ms)
{
    rknn_input inputs[1];
    rknn_output outputs[AI_EXPECT_OUTPUTS];
    std::vector<float> out_scales;
    std::vector<int32_t> out_zps;
    double start_ms;
    int model_w  = lc->info->model_w;
    int model_h  = lc->info->model_h;
    int model_c  = lc->info->model_c;
    int n_output = lc->info->io_num.n_output;
    int ret;

    /* 输入准备 */
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index        = 0;
    inputs[0].type         = RKNN_TENSOR_UINT8;
    inputs[0].size         = (uint32_t)(model_w * model_h * model_c);
    inputs[0].fmt          = RKNN_TENSOR_NHWC;
    inputs[0].pass_through = 0;
    inputs[0].buf          = resized.data;

    start_ms = get_time_ms();
    ret      = rknn_inputs_set(lc->ctx, 1, inputs);
    if (ret < 0) {
        LOG_E("AI", "rknn_inputs_set 失败 ret=%d", ret);
        return false;
    }
    ret = rknn_run(lc->ctx, NULL);
    if (ret < 0) {
        LOG_E("AI", "rknn_run 失败 ret=%d", ret);
        return false;
    }

    /* 输出获取 */
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < n_output; i++) {
        outputs[i].index      = i;
        outputs[i].want_float = 0;
    }
    ret = rknn_outputs_get(lc->ctx, n_output, outputs, NULL);
    if (ret < 0) {
        LOG_E("AI", "rknn_outputs_get 失败 ret=%d", ret);
        return false;
    }
    *infer_ms = get_time_ms() - start_ms;

    /* 后处理（NMS），量化参数来自输出属性 */
    for (int i = 0; i < n_output; i++) {
        out_scales.push_back(lc->out_attrs[i].scale);
        out_zps.push_back(lc->out_attrs[i].zp);
    }
    memset(result_group, 0, sizeof(*result_group));
    if (0 != post_process((int8_t*)outputs[0].buf,
                          (int8_t*)outputs[1].buf,
                          (int8_t*)outputs[2].buf,
                          model_h, model_w,
                          BOX_THRESH, NMS_THRESH,
                          preproc->pads, preproc->scale, preproc->scale,
                          out_zps, out_scales, result_group,
                          lc->worker->labels_path)) {
        LOG_W("AI", "后处理失败（labels 加载异常）");
    }

    /* 降频打印（DEBUG 级：阈值以下整体跳过，连拼接都省） */
    if ((LOG_LVL_DEBUG <= LOG_LEVEL) &&
        ((0 < result_group->count) ||
         (0 == lc->frame_index % AI_LOG_INTERVAL))) {
        if (0 < result_group->count) {
            printf("[AI][D] 检测到 %d 个目标: ", result_group->count);
            for (int i = 0; i < result_group->count; i++) {
                detect_result_t* r = &result_group->results[i];
                printf("%s %.0f%%@(%d,%d,%d,%d) ",
                       r->name, r->prop * 100,
                       r->box.left, r->box.top,
                       r->box.right, r->box.bottom);
            }
            printf("| 推理 %.1f ms\n", *infer_ms);
        } else {
            printf("[AI][D] 无目标 | 推理 %.1f ms\n", *infer_ms);
        }
    }

    rknn_outputs_release(lc->ctx, n_output, outputs);
    return true;
}

/*****************************************************************************
 * 函数名称：loop_person_state
 * 功能描述：紧急录像联动状态机——person 连续 3 帧锁定最新分段
 * 输入参数：@lc           - 推理循环上下文
 *           @result_group - 本帧检测结果
 * 注意事项：双重防重复——连续 15 帧无 person 才复位事件标志（防抖动），
 *           locked_path 记忆同段只锁一次（防重复）
 *****************************************************************************/
static void loop_person_state(ai_loop_ctx_t* lc,
                              const detect_result_group_t* result_group)
{
    /* 检测结果推给 UI 画框（帧号关联，UI 按最新结果绘制） */
    loop_push_detect(lc, result_group);

    if (result_has_person(result_group)) {
        lc->no_person_streak = 0;
        lc->person_streak++;
    } else {
        lc->person_streak = 0;
        lc->no_person_streak++;
        if (AI_PERSON_CLEAR_STREAK <= lc->no_person_streak) {
            lc->is_person_locked = false; /* 事件结束，允许下一轮触发 */
        }
    }

    /* 记忆的已锁文件若已被循环覆盖删除，允许同路径新文件再锁 */
    if (('\0' != lc->locked_path[0]) &&
        (0 != access(lc->locked_path, F_OK))) {
        lc->locked_path[0] = '\0';
    }

    if ((AI_PERSON_LOCK_STREAK <= lc->person_streak) &&
        (false == lc->is_person_locked) && (NULL != lc->worker->file_mgr)) {
        char latest_path[FILE_PATH_MAX];

        /* 原子"取最新分段+锁定"（单次持锁无 TOCTOU）；
         * 同段录像只锁一次（locked_path 记忆），分段切换后新段可再锁 */
        if ((0 == file_mgr_lock_latest(lc->worker->file_mgr, latest_path,
                                       sizeof(latest_path))) &&
            (0 != strcmp(latest_path, lc->locked_path))) {
            LOG_I("AI", "person 连续 %d 帧 → 紧急锁定 %s",
                    lc->person_streak, latest_path);
            lc->is_person_locked = true;
            lc->person_streak    = 0;
            strncpy(lc->locked_path, latest_path,
                    sizeof(lc->locked_path) - 1);
            lc->locked_path[sizeof(lc->locked_path) - 1] = '\0';
        }
    }
}

/*****************************************************************************
 * 函数名称：inference_loop
 * 功能描述：推理主循环——取帧、预处理、NPU 推理、后处理、帧共享
 * 输入参数：@worker     - AI 线程上下文
 *           @ctx        - RKNN 上下文
 *           @info       - 模型信息
 *           @out_attrs  - 输出属性数组
 * 注意事项：循环由 running 标志控制退出；各阶段拆分为独立函数
 *****************************************************************************/
static void inference_loop(ai_worker_t* worker, rknn_context ctx,
                           const rknn_model_info_t* info,
                           const rknn_tensor_attr* out_attrs)
{
    cv::Mat frame(PREVIEW_HEIGHT, PREVIEW_WIDTH, CV_8UC3);
    cv::Mat resized(info->model_h, info->model_w, CV_8UC3);
    ai_loop_ctx_t lc;
    preproc_result_t preproc;
    detect_result_group_t result_group;
    double infer_ms;

    memset(&lc, 0, sizeof(lc));
    lc.worker    = worker;
    lc.ctx       = ctx;
    lc.info      = info;
    lc.out_attrs = out_attrs;

    LOG_I("AI", "线程启动，帧源: GStreamer tee 预览分支");

    while (*(worker->running)) {
        memset(&preproc, 0, sizeof(preproc));
        memset(&result_group, 0, sizeof(result_group));
        infer_ms = 0.0;

        /* ① 取帧 + 画框 + 推 UI（无新帧则休眠等待，防忙等烧 CPU） */
        if (!loop_capture_show(&lc, frame)) {
            usleep(AI_IDLE_WAIT_US);
            goto next_frame;
        }

        /* ② 预处理 + 推理 + 后处理（RKNN 失败跳到下一轮） */
        preproc = loop_preprocess(&lc, frame, resized);
        if (!loop_run_inference(&lc, resized, &preproc,
                                &result_group, &infer_ms)) {
            goto next_frame;
        }

        /* ③ 紧急录像联动状态机 */
        loop_person_state(&lc, &result_group);

    next_frame:;
    }
}

/*****************************************************************************
 * 函数名称：ai_worker_entry
 * 功能描述：AI 推理线程入口
 * 输入参数：@arg - ai_worker_t 指针
 * 返回值：  NULL
 *****************************************************************************/
void* ai_worker_entry(void* arg)
{
    ai_worker_t* worker       = (ai_worker_t*)arg;
    unsigned char* model_data = NULL;
    rknn_model_info_t model_info;
    rknn_context ctx            = 0;
    rknn_tensor_attr* out_attrs = NULL;
    int model_size              = 0;

    if (NULL == worker) {
        return NULL;
    }
    memset(&model_info, 0, sizeof(model_info));

    /* 加载模型 */
    if (0 != load_model(worker->model_path, &model_data, &model_size)) {
        return NULL;
    }

    /* 初始化 RKNN */
    if (0 != init_rknn(&ctx, model_data, model_size,
                       &model_info, &out_attrs)) {
        free(model_data);
        return NULL;
    }

    LOG_I("AI", "线程启动，模型输入 %dx%d, 输出 %d 个",
            model_info.model_w, model_info.model_h,
            model_info.io_num.n_output);

    /* 推理主循环 */
    inference_loop(worker, ctx, &model_info, out_attrs);

    LOG_I("AI", "线程退出");
    free(out_attrs);
    rknn_destroy(ctx);
    free(model_data);
    return NULL;
}
