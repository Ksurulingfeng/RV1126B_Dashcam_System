/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：thread_mgr.h
 * 文件功能：线程注册表 —— 统一管理各业务线程的启动与回收
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#ifndef THREAD_MGR_H
#define THREAD_MGR_H

#include <pthread.h>
#include <stdbool.h>

/* 最大线程数 */
#define THREAD_MAX 8

/* 线程描述 */
typedef struct {
    char    name[32];              /* 线程名（日志用） */
    void   *(*entry)(void *);      /* 线程入口函数 */
    void    *arg;                  /* 传给入口的参数 */
    pthread_t tid;                 /* 创建后的线程 ID */
    bool     is_started;           /* 是否已创建 */
} thread_info_t;

/* 线程注册表 */
typedef struct {
    thread_info_t threads[THREAD_MAX];
    int           count;
} thread_mgr_t;

/*****************************************************************************
 * 函数名称：thread_mgr_add
 * 功能描述：注册一个线程（不启动，start_all 时统一启动）
 * 输入参数：@mgr   - 注册表
 *           @name  - 线程名
 *           @entry - 线程入口函数
 *           @arg   - 入口参数
 * 返回值：  成功返回0，满或参数错误返回-1
 *****************************************************************************/
int thread_mgr_add(thread_mgr_t *mgr, const char *name,
                   void *(*entry)(void *), void *arg);

/*****************************************************************************
 * 函数名称：thread_mgr_start_all
 * 功能描述：创建并启动全部已注册线程
 * 输入参数：@mgr - 注册表
 * 返回值：  成功返回0，失败返回-1
 *****************************************************************************/
int thread_mgr_start_all(thread_mgr_t *mgr);

/*****************************************************************************
 * 函数名称：thread_mgr_join_all
 * 功能描述：等待全部线程退出（线程检查自己的退出条件后 return）
 * 输入参数：@mgr - 注册表
 *****************************************************************************/
void thread_mgr_join_all(thread_mgr_t *mgr);

#endif /* THREAD_MGR_H */
