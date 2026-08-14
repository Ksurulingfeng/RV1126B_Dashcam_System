/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：nmea_parser.c
 * 文件功能：NMEA 0183 协议解析实现
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#define _GNU_SOURCE   /* strtok_r 等 POSIX 函数 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nmea_parser.h"

#define NMEA_MAX_FIELD   32     /* 一行最多 32 个字段 */
#define NMEA_MAX_LINE    128    /* 一行最大长度 */

/*****************************************************************************
 * 函数名称：nmea_init
 * 功能描述：初始化 GPS 数据结构，全字段清零
 * 输入参数：@data - GPS 数据结构指针
 *****************************************************************************/
void nmea_init(gps_data_t *data)
{
    if (NULL != data) {
        memset(data, 0, sizeof(*data));
    }
}


/*****************************************************************************
 * 函数名称：coord_to_degrees
 * 功能描述：NMEA 坐标（ddmm.mmmm 分格式）转十进制角度
 * 输入参数：@raw  - 原始坐标字符串，如 "2234.5678"
 *           @hemi - 半球字母，'N'/'S'/'E'/'W'
 * 返回值：  十进制角度（南纬/西经为负值）
 *****************************************************************************/
static double coord_to_degrees(const char *raw, char hemi)
{
    double value;
    double degrees;
    double minutes;
    double result;

    if ((NULL == raw) || ('\0' == raw[0])) {
        return 0.0;
    }

    value = atof(raw);

    /* ddmm.mmmm → 度.分 */
    degrees = (double)((int)(value / 100.0));
    minutes = value - (degrees * 100.0);

    result = degrees + minutes / 60.0;

    /* 南纬、西经为负 */
    if (('S' == hemi) || ('W' == hemi)) {
        result = -result;
    }

    return result;
}


/*****************************************************************************
 * 函数名称：parse_gga
 * 功能描述：解析 GGA 语句（定位质量、卫星数、经纬度）
 * 输入参数：@fields - 逗号切分后的字段数组
 * 输出参数：@out    - 解析结果
 *****************************************************************************/
static void parse_gga(char *fields[], gps_data_t *out)
{
    /* $GPGGA,time,lat,NS,lon,EW,quality,sats,... */
    out->latitude    = coord_to_degrees(fields[2], fields[3][0]);
    out->longitude   = coord_to_degrees(fields[4], fields[5][0]);
    out->fix_quality = atoi(fields[6]);
    out->satellites  = atoi(fields[7]);
    out->has_gga     = true;
    out->is_valid    = (out->fix_quality > 0);
}


/*****************************************************************************
 * 函数名称：parse_rmc
 * 功能描述：解析 RMC 语句（速度、UTC 时间）
 * 输入参数：@fields - 逗号切分后的字段数组
 * 输出参数：@out    - 解析结果
 *****************************************************************************/
static void parse_rmc(char *fields[], gps_data_t *out)
{
    char time_str[16];
    double speed_knots;

    /* $GPRMC,time,status,lat,NS,lon,EW,speed_knots,... */
    speed_knots = atof(fields[7]);
    out->speed_kmh = speed_knots * 1.852;   /* 节 → 公里/小时 */

    /* UTC 时间 HHMMSS.ss → 时分秒 */
    strncpy(time_str, fields[1], sizeof(time_str) - 1);
    time_str[sizeof(time_str) - 1] = '\0';

    if (strlen(time_str) >= 6) {
        char hh[3] = {time_str[0], time_str[1], '\0'};
        char mm[3] = {time_str[2], time_str[3], '\0'};
        char ss[3] = {time_str[4], time_str[5], '\0'};

        out->hour   = atoi(hh);
        out->minute = atoi(mm);
        out->second = atoi(ss);
    }

    out->has_rmc = true;
}


/*****************************************************************************
 * 函数名称：nmea_parse_line
 * 功能描述：解析一行 NMEA 语句，自动识别 GGA/RMC
 * 输入参数：@line - 一行原始 NMEA 文本
 * 输出参数：@out  - 解析结果
 * 返回值：  成功0，非 GGA/RMC 或格式错误返回-1
 *****************************************************************************/
int nmea_parse_line(const char *line, gps_data_t *out)
{
    char  buf[NMEA_MAX_LINE];
    char *fields[NMEA_MAX_FIELD];
    int   field_count = 0;
    char *p;
    char *field_start;

    if ((NULL == line) || (NULL == out)) {
        return -1;
    }

    /* 拷贝一份：解析过程会修改字符串 */
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* 手动按逗号切分（保留空字段：strtok 会吞掉连续逗号） */
    p = buf;
    field_start = buf;
    while ((NULL != p) && ('\0' != *p) && (field_count < NMEA_MAX_FIELD)) {
        if (',' == *p) {
            *p = '\0';
            fields[field_count++] = field_start;
            field_start = p + 1;
        }
        p++;
    }
    /* 最后一个字段（到校验和或行尾） */
    if (field_count < NMEA_MAX_FIELD) {
        fields[field_count++] = field_start;
    }

    /* 去掉行尾校验和 *xx */
    if (field_count > 0) {
        char *star = strchr(fields[field_count - 1], '*');
        if (NULL != star) {
            *star = '\0';
        }
    }

    if (field_count < 1) {
        return -1;
    }

    /* 按语句类型分发 */
    if (0 == strncmp(fields[0], "$GPGGA", 6)) {
        if (field_count < 8) {
            return -1;
        }
        parse_gga(fields, out);
        return 0;
    }

    if (0 == strncmp(fields[0], "$GPRMC", 6)) {
        if (field_count < 8) {
            return -1;
        }
        parse_rmc(fields, out);
        return 0;
    }

    /* 其他语句（GSA/GSV/VTG 等）忽略 */
    return -1;
}
