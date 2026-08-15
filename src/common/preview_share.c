/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：preview_share.c
 * 文件功能：预览帧共享实现（多消费者模式，格式无关）
 * 作    者：heifast
 * 创建日期：2026-08-15
 *****************************************************************************/

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "preview_share.h"

/*****************************************************************************
 * 函数名称：preview_share_init
 * 功能描述：初始化互斥锁并按指定帧大小分配缓冲
 * 输入参数：@ps   - 共享缓冲指针
 *           @size - 帧字节数
 * 注意事项：分配失败时 buf 置 NULL 并告警，push/pop 会静默跳过；
 *           与 preview_share_deinit 配对使用
 *****************************************************************************/
void preview_share_init(preview_share_t *ps, uint32_t size)
{
    if ((NULL == ps) || (0 == size)) {
        return;
    }

    pthread_mutex_init(&ps->mutex, NULL);
    ps->frame_id = 0;
    ps->size = size;
    ps->buf = (uint8_t *)malloc(size);
    if (NULL == ps->buf) {
        LOG_W("PREVIEW", "帧缓冲分配失败(size=%u)，预览将无画面", size);
    } else {
        memset(ps->buf, 0, size);
    }
}

/*****************************************************************************
 * 函数名称：preview_share_deinit
 * 功能描述：销毁互斥锁并释放帧缓冲
 * 输入参数：@ps - 共享缓冲指针
 * 注意事项：必须在所有消费者停止访问后调用；与 init 配对
 *****************************************************************************/
void preview_share_deinit(preview_share_t *ps)
{
    if (NULL == ps) {
        return;
    }

    if (NULL != ps->buf) {
        free(ps->buf);
        ps->buf = NULL;
    }
    ps->size = 0;
    pthread_mutex_destroy(&ps->mutex);
}

/*****************************************************************************
 * 函数名称：preview_share_push
 * 功能描述：GStreamer appsink 回调写入新帧（锁内拷贝，帧序号递增）
 * 输入参数：@ps  - 共享缓冲指针
 *           @data - 帧数据
 *****************************************************************************/
void preview_share_push(preview_share_t *ps, const uint8_t *data)
{
    if ((NULL == ps) || (NULL == data) || (NULL == ps->buf)) {
        return;
    }

    pthread_mutex_lock(&ps->mutex);
    memcpy(ps->buf, data, ps->size);
    ps->frame_id++;
    pthread_mutex_unlock(&ps->mutex);
}

/*****************************************************************************
 * 函数名称：preview_share_pop
 * 功能描述：读取最新帧（多消费者安全：各消费者维护自己的 frame_id）
 * 输入参数：@ps       - 共享缓冲指针
 * 输出参数：@out      - 拷贝出的帧数据（调用者分配 size 字节）
 *           @frame_id - 输入：本消费者上次读到的序号；
 *                       输出：更新为本次读到的序号
 * 返回值：  有新帧返回 true，无新帧返回 false
 *****************************************************************************/
bool preview_share_pop(preview_share_t *ps, uint8_t *out,
                       uint32_t *frame_id)
{
    bool has_new = false;

    if ((NULL == ps) || (NULL == out) || (NULL == frame_id) ||
        (NULL == ps->buf)) {
        return false;
    }

    pthread_mutex_lock(&ps->mutex);
    if (*frame_id != ps->frame_id) {
        memcpy(out, ps->buf, ps->size);
        *frame_id = ps->frame_id;
        has_new = true;
    }
    pthread_mutex_unlock(&ps->mutex);

    return has_new;
}
