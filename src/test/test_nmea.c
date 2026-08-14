/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：test_nmea.c
 * 文件功能：nmea_parser 模块单元测试（PC 端运行，假数据验证）
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#include <stdio.h>
#include <math.h>

#include "nmea_parser.h"

/* 广州坐标：北纬 23.1291°，东经 113.2644°
 * ddmm.mmmm 格式：纬度 2307.7460 (23°07.7460')，经度 11315.8640
 */
#define TEST_GGA "$GPGGA,091500.00,2307.7460,N,11315.8640,E,1,08,1.0,50.0,M,,,,*66"
#define TEST_RMC "$GPRMC,091500.00,A,2307.7460,N,11315.8640,E,10.8,0.0,130826,,*6B"

/* 无定位的 GGA（室内场景） */
#define TEST_GGA_NOFIX "$GPGGA,,,,,,0,,,,,,,,*66"

static int near(double a, double b, double eps)
{
    return fabs(a - b) < eps;
}

int main(void)
{
    gps_data_t gps;
    int ret;
    int failed = 0;

    printf("===== 场景1: 解析 GGA（有定位） =====\n");
    nmea_init(&gps);
    ret = nmea_parse_line(TEST_GGA, &gps);
    printf("  ret=%d (期望0)\n", ret);
    printf("  纬度: %.6f (期望 23.1291)\n", gps.latitude);
    printf("  经度: %.6f (期望 113.2644)\n", gps.longitude);
    printf("  卫星数: %d (期望 8)\n", gps.satellites);
    printf("  定位质量: %d (期望 1)\n", gps.fix_quality);
    printf("  有效: %d (期望 1)\n", gps.is_valid);
    if ((0 == ret) && near(gps.latitude, 23.1291, 0.001) &&
        near(gps.longitude, 113.2644, 0.001) &&
        (8 == gps.satellites) && (1 == gps.fix_quality) && gps.is_valid) {
        printf("PASS\n\n");
    } else {
        printf("FAIL\n\n");
        failed++;
    }

    printf("===== 场景2: 解析 RMC（速度+时间） =====\n");
    nmea_init(&gps);
    ret = nmea_parse_line(TEST_RMC, &gps);
    printf("  速度: %.1f km/h (期望 20.0)\n", gps.speed_kmh);
    printf("  时间: %02d:%02d:%02d (期望 09:15:00)\n",
           gps.hour, gps.minute, gps.second);
    if ((0 == ret) && near(gps.speed_kmh, 20.0, 0.1) &&
        (9 == gps.hour) && (15 == gps.minute) && (0 == gps.second)) {
        printf("PASS\n\n");
    } else {
        printf("FAIL\n\n");
        failed++;
    }

    printf("===== 场景3: 无定位 GGA =====\n");
    nmea_init(&gps);
    ret = nmea_parse_line(TEST_GGA_NOFIX, &gps);
    printf("  ret=%d (期望0)\n", ret);
    printf("  有效: %d (期望 0)\n", gps.is_valid);
    if ((0 == ret) && !gps.is_valid) {
        printf("PASS\n\n");
    } else {
        printf("FAIL\n\n");
        failed++;
    }

    printf("===== 场景4: 非 GGA/RMC 语句应忽略 =====\n");
    nmea_init(&gps);
    ret = nmea_parse_line("$GPGSV,2,1,08,01,40,083,46,*70", &gps);
    printf("  ret=%d (期望 -1)\n", ret);
    if (-1 == ret) {
        printf("PASS\n\n");
    } else {
        printf("FAIL\n\n");
        failed++;
    }

    printf("===== 场景5: 顺序解析 GGA+RMC 数据合并 =====\n");
    nmea_init(&gps);
    nmea_parse_line(TEST_GGA, &gps);
    nmea_parse_line(TEST_RMC, &gps);
    printf("  纬度: %.6f, 速度: %.1f, 时间: %02d:%02d:%02d, 卫星: %d\n",
           gps.latitude, gps.speed_kmh, gps.hour, gps.minute, gps.second,
           gps.satellites);
    if (gps.has_gga && gps.has_rmc && gps.is_valid) {
        printf("PASS\n\n");
    } else {
        printf("FAIL\n\n");
        failed++;
    }

    if (0 == failed) {
        printf("===== 全部测试通过 =====\n");
    } else {
        printf("===== %d 个场景失败 =====\n", failed);
    }

    return failed;
}
