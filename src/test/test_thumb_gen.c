/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：test_thumb_gen.c
 * 文件功能：thumb_gen 缩略图生成单元测试（PC 端运行）
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include "thumb_gen.h"

/* 缩略图测试尺寸上限（与 thumb_gen 内部校验一致） */
#define TEST_MAX_DIMENSION 8192


/*****************************************************************************
 * 函数名称：parse_dimension
 * 功能描述：解析尺寸字符串为整数（strtol 带错误检测）
 * 输入参数：@str - 数字字符串
 * 返回值：  合法返回 0，非法返回 -1
 *****************************************************************************/
static int parse_dimension(const char *str, int *value)
{
    char *end = NULL;
    long val;

    val = strtol(str, &end, 10);
    if ((str == end) || ('\0' != *end) || (0 >= val) ||
        (TEST_MAX_DIMENSION < val)) {
        return -1;
    }
    *value = (int)val;
    return 0;
}


/*****************************************************************************
 * 函数名称：main
 * 功能描述：测试入口 —— 用法：test_thumb_gen <视频> <输出bmp> <宽> <高>
 * 输入参数：@argc/@argv - 命令行参数
 * 返回值：  成功0，失败非0
 *****************************************************************************/
int main(int argc, char* argv[])
{
    int width;
    int height;
    int ret;

    if (5 > argc) {
        fprintf(stderr, "用法: %s <视频文件> <输出bmp> <宽> <高>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if ((0 != parse_dimension(argv[3], &width)) ||
        (0 != parse_dimension(argv[4], &height))) {
        fprintf(stderr, "[FAIL] 尺寸参数非法\n");
        return EXIT_FAILURE;
    }

    printf("输入视频: %s\n", argv[1]);
    printf("输出缩略图: %s (%dx%d)\n", argv[2], width, height);

    ret = thumb_gen_from_video(argv[1], argv[2], width, height);
    if (0 != ret) {
        fprintf(stderr, "[FAIL] 缩略图生成失败\n");
        return EXIT_FAILURE;
    }

    printf("[PASS] 缩略图生成成功\n");
    return EXIT_SUCCESS;
}
