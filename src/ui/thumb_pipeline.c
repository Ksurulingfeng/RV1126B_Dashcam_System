/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：thumb_pipeline.c
 * 文件功能：缩略图后台生成管线实现 —— 单线程串行生成 + 双队列解耦
 * 作    者：heifast
 * 创建日期：2026-08-15
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include "thumb_gen.h"
#include "log.h"
#include "thumb_pipeline.h"

/* 后台生成线程（低优先级，避免与录像编码抢 CPU） */
static pthread_t s_thread;
static volatile bool s_is_running = false;

/* 请求队列（UI 线程写入，生成线程消费） */
static char s_req_queue[THUMB_QUEUE_MAX][FILE_PATH_MAX];
static int s_req_head = 0;
static int s_req_count = 0;
static pthread_mutex_t s_req_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_req_cond = PTHREAD_COND_INITIALIZER;

/* 完成队列（生成线程写入，UI 线程消费） */
static thumb_done_t s_done_queue[THUMB_QUEUE_MAX];
static int s_done_head = 0;
static int s_done_count = 0;
static pthread_mutex_t s_done_mutex = PTHREAD_MUTEX_INITIALIZER;

/*****************************************************************************
 * 函数名称：queue_already_requested
 * 功能描述：检查请求队列是否已包含指定文件（需持有 req 锁）
 * 输入参数：@video_path - 录像路径
 * 返回值：  已存在返回 true，否则 false
 *****************************************************************************/
static bool queue_already_requested(const char* video_path)
{
    int i;

    for (i = 0; i < s_req_count; i++) {
        int idx = (s_req_head + i) % THUMB_QUEUE_MAX;
        if (0 == strcmp(s_req_queue[idx], video_path)) {
            return true;
        }
    }
    return false;
}

/*****************************************************************************
 * 函数名称：build_thumb_path
 * 功能描述：由录像路径构造缩略图 BMP 路径（/tmp/thumbs/文件名.bmp）
 * 输入参数：@video_path - 录像路径
 * 输出参数：@bmp_path   - 输出路径缓冲（FILE_PATH_MAX）
 *****************************************************************************/
static void build_thumb_path(const char* video_path, char* bmp_path)
{
    const char* slash = strrchr(video_path, '/');
    const char* name = (NULL == slash) ? video_path : slash + 1;
    const char* dot = strrchr(name, '.');

    if (NULL == dot) {
        snprintf(bmp_path, FILE_PATH_MAX, "%s/%s.bmp",
                 THUMB_PIPELINE_DIR, name);
    } else {
        snprintf(bmp_path, FILE_PATH_MAX, "%s/%.*s.bmp",
                 THUMB_PIPELINE_DIR, (int)(dot - name), name);
    }
}

/*****************************************************************************
 * 函数名称：thumb_worker_thread
 * 功能描述：生成线程入口——阻塞等请求，串行生成，结果入完成队列
 * 输入参数：@arg - 未使用
 * 返回值：  NULL
 * 注意事项：nice+10 降级；生成失败也入完成队列（ok=false 供 UI 显示占位）
 *****************************************************************************/
static void* thumb_worker_thread(void* arg)
{
    (void)arg;

    /* 不降优先级：缩略图是用户交互任务（等着看），且生成是
     * 秒级短任务；nice 降级在 UI 渲染高负载下会被饿死到分钟级
     * （板端实测 nice(10) 时 2 分钟/张） */
    nice(0);

    while (s_is_running) {
        char video_path[FILE_PATH_MAX];
        char bmp_path[FILE_PATH_MAX];
        thumb_done_t done;
        int ret;

        /* 取一个请求（无请求时阻塞等待） */
        pthread_mutex_lock(&s_req_mutex);
        while ((0 == s_req_count) && s_is_running) {
            pthread_cond_wait(&s_req_cond, &s_req_mutex);
        }
        if (false == s_is_running) {
            pthread_mutex_unlock(&s_req_mutex);
            break;
        }
        strncpy(video_path, s_req_queue[s_req_head], sizeof(video_path) - 1);
        video_path[sizeof(video_path) - 1] = '\0';
        s_req_head = (s_req_head + 1) % THUMB_QUEUE_MAX;
        s_req_count--;
        pthread_mutex_unlock(&s_req_mutex);

        /* 生成缩略图（FFmpeg 解码抽帧，计时诊断性能） */
        build_thumb_path(video_path, bmp_path);
        {
            struct timespec t0;
            struct timespec t1;
            long ms;

            clock_gettime(CLOCK_MONOTONIC, &t0);
            ret = thumb_gen_from_video(video_path, bmp_path,
                                       THUMB_PIPELINE_W, THUMB_H);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ms = (t1.tv_sec - t0.tv_sec) * 1000 +
                 (t1.tv_nsec - t0.tv_nsec) / 1000000;
            LOG_I("THUMB", "生成耗时 %ldms ret=%d path=%s",
                  ms, ret, video_path);
        }

        /* 结果入完成队列（失败也入队，UI 显示占位） */
        memset(&done, 0, sizeof(done));
        strncpy(done.video_path, video_path, sizeof(done.video_path) - 1);
        strncpy(done.bmp_path, bmp_path, sizeof(done.bmp_path) - 1);
        done.ok = (0 == ret);

        pthread_mutex_lock(&s_done_mutex);
        if (s_done_count < THUMB_QUEUE_MAX) {
            int idx = (s_done_head + s_done_count) % THUMB_QUEUE_MAX;
            s_done_queue[idx] = done;
            s_done_count++;
        }
        pthread_mutex_unlock(&s_done_mutex);
    }

    return NULL;
}

/*****************************************************************************
 * 函数名称：thumb_pipeline_init
 * 功能描述：创建缩略图目录并启动后台生成线程
 * 返回值：  成功0，失败-1
 *****************************************************************************/
int thumb_pipeline_init(void)
{
    struct stat st;
    int ret;

    /* 确保缩略图目录存在（/tmp 内存文件系统，重启自动清理） */
    if (0 != stat(THUMB_PIPELINE_DIR, &st)) {
        if (0 != mkdir(THUMB_PIPELINE_DIR, 0755)) {
            LOG_E("THUMB", "创建缩略图目录失败");
            return -1;
        }
    }

    s_is_running = true;
    ret = pthread_create(&s_thread, NULL, thumb_worker_thread, NULL);
    if (0 != ret) {
        LOG_E("THUMB", "生成线程创建失败");
        s_is_running = false;
        return -1;
    }

    LOG_I("THUMB", "生成线程启动（低优先级）");
    return 0;
}

/*****************************************************************************
 * 函数名称：thumb_pipeline_request
 * 功能描述：请求生成指定录像文件的缩略图（异步，线程安全，幂等）
 * 输入参数：@video_path - 录像文件完整路径
 * 返回值：  入队成功0（缩略图已存在或已在队列也返回0），
 *           队列满或参数无效-1
 * 注意事项：UI 每次刷新列表会全量请求，已生成的直接跳过，
 *           保证队列只处理真正缺失的缩略图
 *****************************************************************************/
int thumb_pipeline_request(const char* video_path)
{
    char bmp_path[FILE_PATH_MAX];
    int ret = -1;

    if (NULL == video_path) {
        return -1;
    }

    /* 幂等：缩略图已生成则跳过，不占队列 */
    build_thumb_path(video_path, bmp_path);
    if (0 == access(bmp_path, F_OK)) {
        return 0;
    }

    pthread_mutex_lock(&s_req_mutex);
    if ((s_req_count < THUMB_QUEUE_MAX) &&
        (false == queue_already_requested(video_path))) {
        int idx = (s_req_head + s_req_count) % THUMB_QUEUE_MAX;
        strncpy(s_req_queue[idx], video_path,
                sizeof(s_req_queue[idx]) - 1);
        s_req_queue[idx][sizeof(s_req_queue[idx]) - 1] = '\0';
        s_req_count++;
        ret = 0;
        pthread_cond_signal(&s_req_cond);
    }
    pthread_mutex_unlock(&s_req_mutex);

    return ret;
}

/*****************************************************************************
 * 函数名称：thumb_pipeline_pop_done
 * 功能描述：取出一个完成项（非阻塞，线程安全）
 * 输出参数：@out - 完成项输出
 * 返回值：  有完成项返回 true，队列空返回 false
 *****************************************************************************/
bool thumb_pipeline_pop_done(thumb_done_t* out)
{
    bool has_item = false;

    if (NULL == out) {
        return false;
    }

    pthread_mutex_lock(&s_done_mutex);
    if (0 < s_done_count) {
        *out = s_done_queue[s_done_head];
        s_done_head = (s_done_head + 1) % THUMB_QUEUE_MAX;
        s_done_count--;
        has_item = true;
    }
    pthread_mutex_unlock(&s_done_mutex);

    return has_item;
}

/*****************************************************************************
 * 函数名称：thumb_pipeline_stop
 * 功能描述：停止生成线程并释放队列（UI 线程退出时调用）
 *****************************************************************************/
void thumb_pipeline_stop(void)
{
    if (false == s_is_running) {
        return;
    }

    s_is_running = false;
    pthread_cond_signal(&s_req_cond);
    pthread_join(s_thread, NULL);

    pthread_mutex_lock(&s_req_mutex);
    s_req_count = 0;
    s_req_head = 0;
    pthread_mutex_unlock(&s_req_mutex);

    pthread_mutex_lock(&s_done_mutex);
    s_done_count = 0;
    s_done_head = 0;
    pthread_mutex_unlock(&s_done_mutex);

    LOG_I("THUMB", "生成线程退出");
}
