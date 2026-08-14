/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：frame_share.c
 * 文件功能：跨线程帧共享缓冲实现
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#include <string.h>

#include "frame_share.h"

/*****************************************************************************
 * 函数名称：frame_share_init
 * 功能描述：初始化互斥锁和缓冲
 * 输入参数：@fs - 共享缓冲指针
 *****************************************************************************/
void frame_share_init(frame_share_t *fs)
{
    if (NULL == fs) {
        return;
    }
    memset(fs, 0, sizeof(*fs));
    pthread_mutex_init(&fs->mutex, NULL);
}


/*****************************************************************************
 * 函数名称：frame_share_push
 * 功能描述：AI 线程写入新帧
 * 输入参数：@fs   - 共享缓冲指针
 *           @bgra - FRAME_SHARE_SIZE 字节的 BGRA 数据
 * 注意事项：锁内仅 memcpy，持锁时间最小化
 *****************************************************************************/
void frame_share_push(frame_share_t *fs, const uint8_t *bgra)
{
    if ((NULL == fs) || (NULL == bgra)) {
        return;
    }

    pthread_mutex_lock(&fs->mutex);
    memcpy(fs->buf, bgra, FRAME_SHARE_SIZE);
    fs->updated = true;
    pthread_mutex_unlock(&fs->mutex);
}


/*****************************************************************************
 * 函数名称：frame_share_pop
 * 功能描述：UI 线程读取新帧
 * 输入参数：@fs  - 共享缓冲指针
 * 输出参数：@out - 拷贝出的帧数据
 * 返回值：  有新帧返回 true，无新帧返回 false
 *****************************************************************************/
bool frame_share_pop(frame_share_t *fs, uint8_t *out)
{
    bool has_new = false;

    if ((NULL == fs) || (NULL == out)) {
        return false;
    }

    pthread_mutex_lock(&fs->mutex);
    if (fs->updated) {
        memcpy(out, fs->buf, FRAME_SHARE_SIZE);
        fs->updated = false;
        has_new = true;
    }
    pthread_mutex_unlock(&fs->mutex);

    return has_new;
}
