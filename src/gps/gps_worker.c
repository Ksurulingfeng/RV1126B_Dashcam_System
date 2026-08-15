/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：gps_worker.c
 * 文件功能：GPS 采集线程实现
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#define _GNU_SOURCE   /* strtok_r 等 POSIX 函数 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

#include "log.h"
#include "gps_worker.h"

#define GPS_BAUDRATE  B9600
#define GPS_BUF_SIZE  256

/*****************************************************************************
 * 函数名称：uart_open
 * 功能描述：打开串口并配置 9600 8N1 原始模式
 * 输入参数：@dev - 串口设备路径，如 "/dev/ttyUSB1"
 * 返回值：  成功返回 fd，失败返回-1
 *****************************************************************************/
static int uart_open(const char *dev)
{
    struct termios tio;
    int fd;

    /* 阻塞模式 + VMIN=0/VTIME 超时：read 最多等 1 秒返回 0，
     * 不用 O_NONBLOCK（它会覆盖 VMIN/VTIME，导致 read 立即返回 EAGAIN） */
    fd = open(dev, O_RDWR | O_NOCTTY);
    if (0 > fd) {
        LOG_E("GPS", "打开 %s 失败: %s\n", dev, strerror(errno));
        return -1;
    }

    memset(&tio, 0, sizeof(tio));
    tio.c_cflag = GPS_BAUDRATE | CS8 | CLOCAL | CREAD;
    tio.c_iflag = IGNPAR;                 /* 忽略校验错误 */
    tio.c_oflag = 0;
    tio.c_lflag = 0;                      /* 非规范模式（原始模式） */
    tio.c_cc[VTIME] = 10;                 /* 读超时 1 秒 */
    tio.c_cc[VMIN]  = 0;

    tcflush(fd, TCIFLUSH);
    if (0 != tcsetattr(fd, TCSANOW, &tio)) {
        LOG_E("GPS", "配置串口失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}


/*****************************************************************************
 * 函数名称：gps_worker_entry
 * 功能描述：GPS 线程入口，循环读行解析
 * 输入参数：@arg - gps_worker_t 指针
 * 返回值：  NULL
 *****************************************************************************/
void *gps_worker_entry(void *arg)
{
    gps_worker_t *worker = (gps_worker_t *)arg;
    char buf[GPS_BUF_SIZE];
    char line[GPS_BUF_SIZE];
    int  line_len = 0;
    int  n;
    int  i;
    int  error_count = 0;

    if (NULL == worker) {
        return NULL;
    }

    pthread_mutex_init(&worker->mutex, NULL);

    worker->fd = uart_open(worker->dev);
    if (0 > worker->fd) {
        LOG_E("GPS", "串口打开失败，线程退出\n");
        return NULL;
    }

    nmea_init(&worker->data);
    LOG_I("GPS", "线程启动");

    while (*(worker->running)) {
        n = read(worker->fd, buf, sizeof(buf) - 1);
        if (0 > n) {
            /* 真错误（如 EC20 拔出）：退避等待，避免忙等空转 */
            error_count++;
            if (1 == error_count % 100) {
                LOG_E("GPS", "串口读取错误（连续 %d 次）\n",
                        error_count);
            }
            usleep(100000);
            continue;
        }
        error_count = 0;

        if (0 == n) {
            continue;   /* 读超时，无数据 */
        }

        /* 逐字符拼行：NMEA 语句以 \n 结尾 */
        for (i = 0; i < n; i++) {
            if (('\n' == buf[i]) || ('\r' == buf[i])) {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    /* 解析这一行（持锁写 data，供 UI 快照读） */
                    pthread_mutex_lock(&worker->mutex);
                    int parse_ret = nmea_parse_line(line, &worker->data);
                    pthread_mutex_unlock(&worker->mutex);

                    if (0 == parse_ret) {
                        /* 只在定位状态变化时打印，避免每秒刷屏 */
                        static int last_valid = -1;
                        static int last_sats = -1;
                        gps_data_t snapshot;
                        gps_worker_get_data(worker, &snapshot);
                        if ((snapshot.is_valid != last_valid) ||
                            (snapshot.satellites != last_sats)) {
                            LOG_I("GPS", "定位: %.6f, %.6f 卫星%d 质量%d %s\n",
                                   snapshot.latitude, snapshot.longitude,
                                   snapshot.satellites, snapshot.fix_quality,
                                   snapshot.is_valid ? "有效" : "未定位");
                            last_valid = snapshot.is_valid;
                            last_sats = snapshot.satellites;
                        }
                    }
                    line_len = 0;
                }
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = buf[i];
            }
        }
    }

    close(worker->fd);
    worker->fd = -1;
    LOG_I("GPS", "线程退出");
    return NULL;
}


/*****************************************************************************
 * 函数名称：gps_worker_get_data
 * 功能描述：获取定位数据快照（线程安全）
 * 输入参数：@worker - GPS 线程上下文
 * 输出参数：@out    - 数据快照
 * 返回值：  成功返回0，失败返回-1
 *****************************************************************************/
int gps_worker_get_data(gps_worker_t *worker, gps_data_t *out)
{
    if ((NULL == worker) || (NULL == out)) {
        return -1;
    }

    pthread_mutex_lock(&worker->mutex);
    *out = worker->data;
    pthread_mutex_unlock(&worker->mutex);

    return 0;
}
