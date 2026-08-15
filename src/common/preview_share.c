/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：preview_share.c
 * 文件功能：预览帧共享实现（多消费者模式）
 * 作    者：heifast
 * 创建日期：2026-08-15
 *****************************************************************************/

#include <string.h>

#include "preview_share.h"

/*****************************************************************************
 * 函数名称：preview_share_init
 * 功能描述：初始化互斥锁和缓冲
 * 输入参数：@ps - 共享缓冲指针
 *****************************************************************************/
void preview_share_init(preview_share_t *ps)
{
    if (NULL == ps) {
        return;
    }

    pthread_mutex_init(&ps->mutex, NULL);
    ps->frame_id = 0;
    memset(ps->buf, 0, sizeof(ps->buf));
}

/*****************************************************************************
 * 函数名称：preview_share_push
 * 功能描述：GStreamer appsink 回调写入新帧（锁内拷贝，帧序号递增）
 * 输入参数：@ps  - 共享缓冲指针
 *           @nv12 - PREVIEW_SIZE 字节的 NV12 数据
 *****************************************************************************/
void preview_share_push(preview_share_t *ps, const uint8_t *nv12)
{
    if ((NULL == ps) || (NULL == nv12)) {
        return;
    }

    pthread_mutex_lock(&ps->mutex);
    memcpy(ps->buf, nv12, PREVIEW_SIZE);
    ps->frame_id++;
    pthread_mutex_unlock(&ps->mutex);
}

/*****************************************************************************
 * 函数名称：preview_share_pop
 * 功能描述：读取最新帧（多消费者安全：各消费者维护自己的 frame_id）
 * 输入参数：@ps       - 共享缓冲指针
 * 输出参数：@out      - 拷贝出的 NV12 数据（调用者分配 PREVIEW_SIZE）
 *           @frame_id - 输入：本消费者上次读到的序号；
 *                       输出：更新为本次读到的序号
 * 返回值：  有新帧返回 true，无新帧返回 false
 *****************************************************************************/
bool preview_share_pop(preview_share_t *ps, uint8_t *out,
                       uint32_t *frame_id)
{
    bool has_new = false;

    if ((NULL == ps) || (NULL == out) || (NULL == frame_id)) {
        return false;
    }

    pthread_mutex_lock(&ps->mutex);
    if (*frame_id != ps->frame_id) {
        memcpy(out, ps->buf, PREVIEW_SIZE);
        *frame_id = ps->frame_id;
        has_new = true;
    }
    pthread_mutex_unlock(&ps->mutex);

    return has_new;
}
