/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：detect_share.c
 * 文件功能：检测结果共享实现
 * 作    者：heifast
 * 创建日期：2026-08-15
 *****************************************************************************/

#include <string.h>

#include "detect_share.h"

/*****************************************************************************
 * 函数名称：detect_share_init
 * 功能描述：初始化互斥锁和结果缓冲
 * 输入参数：@ds - 共享缓冲指针
 *****************************************************************************/
void detect_share_init(detect_share_t *ds)
{
    if (NULL == ds) {
        return;
    }

    pthread_mutex_init(&ds->mutex, NULL);
    ds->frame_id = 0;
    ds->count = 0;
}

/*****************************************************************************
 * 函数名称：detect_share_push
 * 功能描述：AI 线程写入一帧检测结果（锁内拷贝）
 * 输入参数：@ds     - 共享缓冲指针
 *           @boxes  - 检测框数组
 *           @count  - 框数量
 *           @frame_id - 对应的预览帧序号
 *****************************************************************************/
void detect_share_push(detect_share_t *ds, const detect_box_t *boxes,
                       uint32_t count, uint32_t frame_id)
{
    if ((NULL == ds) || (NULL == boxes) || (0 == count)) {
        return;
    }

    if (count > DETECT_BOX_MAX) {
        count = DETECT_BOX_MAX;
    }

    pthread_mutex_lock(&ds->mutex);
    memcpy(ds->boxes, boxes, sizeof(detect_box_t) * count);
    ds->count = count;
    ds->frame_id = frame_id;
    pthread_mutex_unlock(&ds->mutex);
}

/*****************************************************************************
 * 函数名称：detect_share_pop
 * 功能描述：UI 线程读取最新检测结果
 * 输入参数：@ds     - 共享缓冲指针
 * 输出参数：@boxes  - 检测框输出数组（调用者分配 DETECT_BOX_MAX）
 *           @count  - 输出框数量
 * 返回值：  有新结果返回 true，无新结果返回 false
 *****************************************************************************/
bool detect_share_pop(detect_share_t *ds, detect_box_t *boxes,
                      uint32_t *count)
{
    bool has_new = false;

    if ((NULL == ds) || (NULL == boxes) || (NULL == count)) {
        return false;
    }

    pthread_mutex_lock(&ds->mutex);
    if (0 < ds->count) {
        memcpy(boxes, ds->boxes, sizeof(detect_box_t) * ds->count);
        *count = ds->count;
        has_new = true;
    }
    pthread_mutex_unlock(&ds->mutex);

    return has_new;
}
