/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：nmea_parser.h
 * 文件功能：NMEA 0183 协议解析 —— GGA/RMC 语句
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#ifndef NMEA_PARSER_H
#define NMEA_PARSER_H

#include <stdbool.h>
#include <stdint.h>

/* GPS 解析结果（多条语句合并后的完整数据） */
typedef struct {
    double  latitude;      /* 纬度（度，北正南负） */
    double  longitude;     /* 经度（度，东正西负） */
    double  speed_kmh;     /* 速度（公里/小时，来自 RMC） */
    int     satellites;    /* 可见卫星数（GGA） */
    int     fix_quality;   /* 定位质量：0=无效 1=GPS 2=DGPS */
    int     hour;          /* UTC 时间（来自 RMC） */
    int     minute;
    int     second;
    bool    has_gga;       /* 本次是否解析到 GGA */
    bool    has_rmc;       /* 本次是否解析到 RMC */
    bool    is_valid;      /* 定位是否有效（fix_quality>0） */
} gps_data_t;

/*****************************************************************************
 * 函数名称：nmea_parse_line
 * 功能描述：解析一行 NMEA 语句，自动识别 GGA/RMC 并填充数据
 * 输入参数：@line - 一行原始 NMEA 文本（如 "$GPGGA,..."）
 * 输出参数：@out  - 解析结果（只更新本语句涉及的字段）
 * 返回值：  解析成功返回0，非 GGA/RMC 或格式错误返回-1
 *****************************************************************************/
int nmea_parse_line(const char *line, gps_data_t *out);

/*****************************************************************************
 * 函数名称：nmea_init
 * 功能描述：初始化 GPS 数据结构
 * 输入参数：@data - GPS 数据结构指针
 *****************************************************************************/
void nmea_init(gps_data_t *data);

#endif /* NMEA_PARSER_H */
