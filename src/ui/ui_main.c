/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：ui_main.c
 * 文件功能：LVGL 图形界面主入口 —— 主页（状态栏/全屏预览/右侧导航）
 *          与页面切换（录像库/设置）
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "display/drm.h"

#include "touch_input.h"
#include "ui_main.h"
#include "ui_pages.h"

/* 中文字体（思源黑体，freetype 渲染） */
#define UI_FONT_PATH "/usr/share/fonts/source-han-sans-cn/SourceHanSansCN-Normal.otf"

/* 主题颜色（简约风格：深灰黑 + 白灰文字，彩色仅 REC 红） */
#define UI_COLOR_BG     0x0E0E12 /* 页面背景（近黑） */
#define UI_COLOR_CARD   0x1C1C1E /* 卡片底色（深灰） */
#define UI_COLOR_REC    0xFF3B30 /* 录制红（唯一彩色） */
#define UI_COLOR_TEXT   0xFFFFFF /* 主文本（白） */
#define UI_COLOR_TEXT2  0x8A8A8E /* 次文本（灰） */

/* 布局常量（1280×720 逻辑分辨率） */
#define UI_STATUS_BAR_H 48        /* 顶部状态栏高度 */
#define UI_NAV_BAR_W    160       /* 右侧导航栏宽度 */
#define UI_NAV_BTN_H    88        /* 导航按钮高度 */

/* 界面控件 */
static lv_obj_t* s_screen_live;
static lv_obj_t* s_label_time;
static lv_obj_t* s_label_gps;
static lv_obj_t* s_canvas;

/* canvas 帧缓冲（720×405 BGRA） */
static lv_color_t* s_canvas_buf;

/* freetype 字体实例 */
static lv_ft_info_t s_ft_info;

/* 页面栈（返回键用，最多 4 级） */
#define UI_PAGE_STACK_MAX 4
static lv_obj_t* s_page_stack[UI_PAGE_STACK_MAX];
static int s_page_stack_top = -1;

/*****************************************************************************
 * 函数名称：ui_status_tick
 * 功能描述：每秒刷新状态栏（时间/GPS/存储）
 * 输入参数：@timer - LVGL 定时器
 *****************************************************************************/
static void ui_status_tick(lv_timer_t* timer)
{
    ui_worker_t* ui = (ui_worker_t*)timer->user_data;
    time_t now;
    struct tm* tm_info;
    char time_buf[32];
    char gps_buf[64];

    if (NULL == ui) {
        return;
    }

    /* 时间 */
    now     = time(NULL);
    tm_info = localtime(&now);
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
    lv_label_set_text(s_label_time, time_buf);

    /* GPS 状态（快照接口读取，避免与 GPS 线程数据竞争） */
    if (NULL != ui->gps) {
        gps_data_t snapshot;
        if ((0 == gps_worker_get_data(ui->gps, &snapshot)) &&
            snapshot.is_valid) {
            snprintf(gps_buf, sizeof(gps_buf),
                     "GPS: %.5f, %.5f  %.0f km/h",
                     snapshot.latitude,
                     snapshot.longitude,
                     snapshot.speed_kmh);
        } else {
            snprintf(gps_buf, sizeof(gps_buf), "GPS: 搜星中");
        }
        lv_label_set_text(s_label_gps, gps_buf);
    }
}

/*****************************************************************************
 * 函数名称：ui_frame_tick
 * 功能描述：33ms 检查一次共享帧缓冲，有新帧刷新 canvas
 * 输入参数：@timer - LVGL 定时器
 *****************************************************************************/
static void ui_frame_tick(lv_timer_t* timer)
{
    ui_worker_t* ui = (ui_worker_t*)timer->user_data;

    if ((NULL == ui) || (NULL == ui->frame_share) || (NULL == s_canvas_buf)) {
        return;
    }

    /* 有新帧 → 拷入 canvas 缓冲并请求重绘 */
    if (frame_share_pop(ui->frame_share, (uint8_t*)s_canvas_buf)) {
        lv_obj_invalidate(s_canvas);
    }
}

/*****************************************************************************
 * 函数名称：rec_breath_exec
 * 功能描述：REC 红点呼吸动画执行回调（透明度往复）
 * 输入参数：@obj - 红点对象
 *           @opa - 当前透明度
 *****************************************************************************/
static void rec_breath_exec(void* obj, int32_t opa)
{
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)opa, 0);
}

/*****************************************************************************
 * 函数名称：ui_page_push
 * 功能描述：页面栈压入（导航跳转前记录当前页）
 * 输入参数：@page - 当前页面
 *****************************************************************************/
static void ui_page_push(lv_obj_t* page)
{
    if (s_page_stack_top < UI_PAGE_STACK_MAX - 1) {
        s_page_stack_top++;
        s_page_stack[s_page_stack_top] = page;
    }
}

/*****************************************************************************
 * 函数名称：ui_page_pop
 * 功能描述：页面栈弹出（返回键用）
 * 返回值：  上一页面，栈空返回 NULL
 *****************************************************************************/
static lv_obj_t* ui_page_pop(void)
{
    lv_obj_t* page = NULL;

    if (0 <= s_page_stack_top) {
        page = s_page_stack[s_page_stack_top];
        s_page_stack_top--;
    }
    return page;
}

/*****************************************************************************
 * 函数名称：ui_nav_to
 * 功能描述：跳转页面（记录当前页到栈，带动画加载目标页）
 * 输入参数：@target - 目标页面
 *****************************************************************************/
static void ui_nav_to(lv_obj_t* target)
{
    ui_page_push(lv_scr_act());
    lv_scr_load_anim(target, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

/*****************************************************************************
 * 函数名称：ui_nav_back
 * 功能描述：返回上一页（栈空时停留主页，防止空指针加载崩溃）
 *****************************************************************************/
static void ui_nav_back(void)
{
    lv_obj_t* prev = ui_page_pop();

    if (NULL == prev) {
        prev = s_screen_live; /* 栈空：已在主页，返回键保持主页 */
    }
    lv_scr_load_anim(prev, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

/*****************************************************************************
 * 函数名称：ui_nav_event_cb
 * 功能描述：右侧导航按钮统一回调
 * 输入参数：@e - 事件对象（user_data 为按钮语义编码）
 *****************************************************************************/
static void ui_nav_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* btn = lv_event_get_target(e);
    ui_worker_t* ui = (ui_worker_t*)lv_event_get_user_data(e);
    intptr_t action = (intptr_t)lv_obj_get_user_data(btn);
    static lv_obj_t* s_screen_library = NULL;
    static lv_obj_t* s_screen_settings = NULL;

    if (LV_EVENT_CLICKED != code) {
        return;
    }

    switch (action) {
        case 0: /* 录像 */
            if (NULL == s_screen_library) {
                s_screen_library = ui_page_library_create(ui);
            }
            ui_page_library_refresh();
            ui_nav_to(s_screen_library);
            break;
        case 1: /* 设置 */
            if (NULL == s_screen_settings) {
                s_screen_settings = ui_page_settings_create(ui);
            }
            ui_nav_to(s_screen_settings);
            break;
        case 2: /* 主页 */
            lv_scr_load_anim(s_screen_live, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                             200, 0, false);
            s_page_stack_top = -1; /* 回主页清空栈 */
            break;
        case 3: /* 返回 */
            ui_nav_back();
            break;
        default:
            break;
    }
}

/*****************************************************************************
 * 函数名称：ui_create_status_bar
 * 功能描述：创建顶部状态栏（三段布局：左 REC/中时间/右 4G·录音·GPS）
 * 输入参数：@parent - 父对象
 *****************************************************************************/
static void ui_create_status_bar(lv_obj_t* parent)
{
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_t* seg_left;
    lv_obj_t* seg_right;
    lv_obj_t* led;
    lv_obj_t* label;
    lv_anim_t anim;

    /* 全宽悬浮状态栏：半透明深灰底，覆盖在预览画面上层 */
    lv_obj_set_size(bar, lv_pct(100), UI_STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左段：REC 红点（呼吸动画）+ REC 文字 */
    seg_left = lv_obj_create(bar);
    lv_obj_set_size(seg_left, LV_SIZE_CONTENT, UI_STATUS_BAR_H);
    lv_obj_set_style_bg_opa(seg_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(seg_left, 0, 0);
    lv_obj_set_flex_flow(seg_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg_left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(seg_left, 8, 0);
    lv_obj_clear_flag(seg_left, LV_OBJ_FLAG_SCROLLABLE);

    led = lv_obj_create(seg_left);
    lv_obj_set_size(led, 14, 14);
    lv_obj_set_style_bg_color(led, lv_color_hex(UI_COLOR_REC), 0);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(led, 0, 0);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, led);
    lv_anim_set_exec_cb(&anim, rec_breath_exec);
    lv_anim_set_values(&anim, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_time(&anim, 600);
    lv_anim_set_playback_time(&anim, 600);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);

    label = lv_label_create(seg_left);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_REC), 0);
    lv_obj_set_style_text_font(label, s_ft_info.font, 0);
    lv_label_set_text(label, "REC");

    /* 中段：时间（主文本） */
    s_label_time = lv_label_create(bar);
    lv_obj_set_style_text_color(s_label_time, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_label_time, s_ft_info.font, 0);
    lv_label_set_text(s_label_time, "00:00:00");

    /* 右段：4G 状态 · 录音状态 · GPS */
    seg_right = lv_obj_create(bar);
    lv_obj_set_size(seg_right, LV_SIZE_CONTENT, UI_STATUS_BAR_H);
    lv_obj_set_style_bg_opa(seg_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(seg_right, 0, 0);
    lv_obj_set_flex_flow(seg_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg_right, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(seg_right, 20, 0);
    lv_obj_clear_flag(seg_right, LV_OBJ_FLAG_SCROLLABLE);

    /* 4G 状态（占位，网络模块接入后动态更新） */
    label = lv_label_create(seg_right);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT2), 0);
    lv_obj_set_style_text_font(label, s_ft_info.font, 0);
    lv_label_set_text(label, "4G 离线");

    /* 录音状态（占位，音频模块接入后动态更新） */
    label = lv_label_create(seg_right);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT2), 0);
    lv_obj_set_style_text_font(label, s_ft_info.font, 0);
    lv_label_set_text(label, "未录音");

    /* GPS（次文本） */
    s_label_gps = lv_label_create(seg_right);
    lv_obj_set_style_text_color(s_label_gps, lv_color_hex(UI_COLOR_TEXT2), 0);
    lv_obj_set_style_text_font(s_label_gps, s_ft_info.font, 0);
    lv_label_set_text(s_label_gps, "GPS: 搜星中");
}

/*****************************************************************************
 * 函数名称：ui_nav_bar_create
 * 功能描述：创建右侧竖排导航栏（录像/设置/主页/返回，各页面共用）
 * 输入参数：@parent - 父对象
 *           @ui     - UI 上下文（按钮回调）
 * 注意事项：全局导航组件，每个页面创建一份（LVGL 对象不可跨父级共享）
 *****************************************************************************/
void ui_nav_bar_create(lv_obj_t* parent, ui_worker_t* ui)
{
    static const char* const nav_texts[] = {"录像", "设置", "主页", "返回"};
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_t* btn;
    lv_obj_t* label;
    int i;

    /* 右侧导航栏：竖排 4 键，顶部与状态栏对齐 */
    lv_obj_set_size(bar, UI_NAV_BAR_W, lv_pct(100));
    lv_obj_align(bar, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 16, 0);
    lv_obj_set_style_pad_row(bar, 12, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 4 个导航按钮（卡片灰底白字） */
    for (i = 0; i < 4; i++) {
        btn = lv_btn_create(bar);
        lv_obj_set_size(btn, UI_NAV_BAR_W - 32, UI_NAV_BTN_H);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_CARD), 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, ui_nav_event_cb, LV_EVENT_CLICKED, ui);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i); /* 语义编码 */

        label = lv_label_create(btn);
        lv_obj_center(label);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(label, s_ft_info.font, 0);
        lv_label_set_text(label, nav_texts[i]);
    }
}

/*****************************************************************************
 * 函数名称：ui_build_live_page
 * 功能描述：构建主页（状态栏 + 全屏预览 + 右侧导航）
 * 输入参数：@ui - UI 上下文
 *****************************************************************************/
static void ui_build_live_page(ui_worker_t* ui)
{
    lv_obj_t* screen;

    screen = lv_obj_create(NULL);
    s_screen_live = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), 0);

    /* 全屏预览 canvas（1280×720 铺满屏幕，底层，先创建保证 z 序最底） */
    s_canvas     = lv_canvas_create(screen);
    s_canvas_buf = (lv_color_t*)malloc(FRAME_SHARE_SIZE);
    if (NULL == s_canvas_buf) {
        fprintf(stderr, "[UI] canvas 缓冲分配失败\n");
        return;
    }
    memset(s_canvas_buf, 0, FRAME_SHARE_SIZE);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf,
                         FRAME_SHARE_WIDTH, FRAME_SHARE_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);

    /* 顶部状态栏（悬浮在预览上层） */
    ui_create_status_bar(screen);

    /* 右侧导航栏（悬浮在预览上层） */
    ui_nav_bar_create(screen, ui);

    /* 状态刷新（1s）+ 帧刷新（33ms） */
    lv_timer_create(ui_status_tick, 1000, ui);
    lv_timer_create(ui_frame_tick, 33, ui);
}

/*****************************************************************************
 * 函数名称：ui_worker_entry
 * 功能描述：UI 线程入口
 * 输入参数：@arg - ui_worker_t 指针
 * 返回值：  NULL
 * 注意事项：LVGL 非线程安全，所有 lv_ 调用都在本线程内
 *****************************************************************************/
void* ui_worker_entry(void* arg)
{
    ui_worker_t* ui = (ui_worker_t*)arg;

    if (NULL == ui) {
        return NULL;
    }

    /* LVGL 初始化 + DRM 显示驱动（正点原子适配，自动探测分辨率） */
    lv_init();
    drm_disp_drv_init(0, 0, 90); /* 旋转90° */
    printf("[INFO] UI 线程启动（DRM 后端）\n");

    /* 触摸输入注册（Goodix 电容屏，失败不影响显示） */
    if (0 != touch_input_init()) {
        fprintf(stderr, "[UI] 触摸输入不可用，界面仅作显示\n");
    }

    /* 中文字体初始化（freetype 渲染思源黑体，页面共享） */
    s_ft_info.name   = UI_FONT_PATH;
    s_ft_info.weight = 28;
    s_ft_info.style  = FT_FONT_STYLE_NORMAL;
    s_ft_info.mem    = NULL;
    if (!lv_ft_font_init(&s_ft_info)) {
        fprintf(stderr, "[UI] 中文字体初始化失败，汉字将无法显示\n");
    }
    ui->font = s_ft_info.font;

    /* 构建主页 */
    ui_build_live_page(ui);
    lv_scr_load(s_screen_live);

    /* UI 主循环：按 LVGL 下一个定时器到期时间睡眠，避免空转 */
    while (*(ui->running)) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms > 10) {
            wait_ms = 10; /* 帧刷新 33ms 定时器需要较快响应 */
        }
        usleep(wait_ms * 1000);
    }

    /* 释放 canvas 缓冲 */
    if (NULL != s_canvas_buf) {
        free(s_canvas_buf);
        s_canvas_buf = NULL;
    }

    drm_disp_drv_deinit();
    printf("[INFO] UI 线程退出\n");
    return NULL;
}
