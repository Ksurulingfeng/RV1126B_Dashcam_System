/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：gps_worker.h
 * 文件功能：GPS 采集线程 —— 读串口、解析 NMEA、对外提供定位数据
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#ifndef GPS_WORKER_H
#define GPS_WORKER_H

#include <pthread.h>
#include <stdbool.h>

#include "nmea_parser.h"

/* GPS 线程上下文（线程安全：data 通过快照接口访问） */
typedef struct {
    int            fd;            /* 串口文件描述符 */
    char           dev[64];       /* 串口设备路径，如 "/dev/ttyUSB1" */
    volatile bool *running;       /* 退出标志（main 的信号处理设置） */
    pthread_mutex_t mutex;        /* data 保护锁 */
    gps_data_t     data;          /* 最新定位数据（内部受锁保护） */
} gps_worker_t;

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * 函数名称：gps_worker_get_data
 * 功能描述：获取定位数据快照（线程安全）
 * 输入参数：@worker - GPS 线程上下文
 * 输出参数：@out    - 数据快照
 * 返回值：  成功返回0，失败返回-1
 *****************************************************************************/
int gps_worker_get_data(gps_worker_t *worker, gps_data_t *out);

/*****************************************************************************
 * 函数名称：gps_worker_entry
 * 功能描述：GPS 线程入口（pthread_create 用）
 * 输入参数：@arg - gps_worker_t 指针
 * 返回值：  NULL
 *****************************************************************************/
void *gps_worker_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* GPS_WORKER_H */
