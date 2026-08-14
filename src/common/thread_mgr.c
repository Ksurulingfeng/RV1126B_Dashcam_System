/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：thread_mgr.c
 * 文件功能：线程注册表实现
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "thread_mgr.h"

/*****************************************************************************
 * 函数名称：thread_mgr_add
 * 功能描述：注册线程（不启动）
 * 输入参数：@mgr   - 注册表
 *           @name  - 线程名
 *           @entry - 线程入口
 *           @arg   - 入口参数
 * 返回值：  成功0，注册表满或参数错误-1
 *****************************************************************************/
int thread_mgr_add(thread_mgr_t *mgr, const char *name,
                   void *(*entry)(void *), void *arg)
{
    thread_info_t *info;

    if ((NULL == mgr) || (NULL == name) || (NULL == entry)) {
        return -1;
    }

    if (mgr->count >= THREAD_MAX) {
        fprintf(stderr, "[thread_mgr] 线程表已满(%d)\n", THREAD_MAX);
        return -1;
    }

    info = &mgr->threads[mgr->count];
    memset(info, 0, sizeof(*info));
    strncpy(info->name, name, sizeof(info->name) - 1);
    info->entry = entry;
    info->arg   = arg;

    mgr->count++;
    return 0;
}


/*****************************************************************************
 * 函数名称：thread_mgr_start_all
 * 功能描述：创建全部已注册线程
 * 输入参数：@mgr - 注册表
 * 返回值：  成功0，失败-1
 *****************************************************************************/
int thread_mgr_start_all(thread_mgr_t *mgr)
{
    if (NULL == mgr) {
        return -1;
    }

    for (int i = 0; i < mgr->count; i++) {
        thread_info_t *info = &mgr->threads[i];

        if (0 != pthread_create(&info->tid, NULL, info->entry, info->arg)) {
            fprintf(stderr, "[thread_mgr] 创建线程 %s 失败\n", info->name);
            return -1;
        }
        info->is_started = true;
        printf("[INFO] 线程 %s 已启动\n", info->name);
    }

    return 0;
}


/*****************************************************************************
 * 函数名称：thread_mgr_join_all
 * 功能描述：等待所有已启动线程退出
 * 输入参数：@mgr - 注册表
 *****************************************************************************/
void thread_mgr_join_all(thread_mgr_t *mgr)
{
    int i;

    if (NULL == mgr) {
        return;
    }

    for (i = 0; i < mgr->count; i++) {
        thread_info_t *info = &mgr->threads[i];

        if (info->is_started) {
            pthread_join(info->tid, NULL);
            printf("[INFO] 线程 %s 已退出\n", info->name);
            info->is_started = false;
        }
    }
}
