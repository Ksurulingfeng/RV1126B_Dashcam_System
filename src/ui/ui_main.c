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

#include "log.h"
#include "settings.h"
#include "thumb_pipeline.h"
#include "touch_input.h"
#include "ui_main.h"
#include "ui_pages.h"
#include "ui_theme.h"

/* 中文字体（思源黑体，freetype 渲染） */
#define UI_FONT_PATH "/usr/share/fonts/source-han-sans-cn/SourceHanSansCN-Normal.otf"

/* 布局常量（1280×720 逻辑分辨率） */
#define UI_STATUS_BAR_H 48        /* 顶部状态栏高度 */
#define UI_NAV_BAR_W    160       /* 右侧导航栏宽度 */
#define UI_NAV_BTN_H    88        /* 导航按钮高度 */

/* 检测框标签（类别名 + 置信度，画在框顶） */
#define UI_LABEL_H 20                  /* 标签高度（px） */
#define UI_LABEL_TEXT_MAX \
    (DETECT_NAME_MAX + 8)              /* "person 100%" + 结束符 */

/* 界面控件 */
static lv_obj_t* s_screen_live;
static lv_obj_t* s_label_time;
static lv_obj_t* s_label_gps;
static lv_obj_t* s_canvas;

/* canvas 帧缓冲（1280×720 BGRA，全屏预览）。
 * 依赖 LV_COLOR_DEPTH=32 使 lv_color_t 为 4 字节 BGRA 布局，
 * 与 DRM 后端（disp_bit 32）及 GStreamer BGRA 输出三者保持一致，
 * 任意一方变更需同步检查此耦合 */
static lv_color_t* s_canvas_buf;

/* 预览帧缓冲（BGRA，GStreamer 管线内 videoconvert 已转换，UI 零转换） */
static uint8_t s_bgra_buf[PREVIEW_BGRA_SIZE];

/* UI 已读预览帧序号（多消费者模式，与 AI 线程各自独立） */
static uint32_t s_ui_frame_id = 0;

/* freetype 字体实例 */
static lv_ft_info_t s_ft_info;

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
 * 函数名称：ui_draw_box_label
 * 功能描述：在检测框顶部绘制标签（深色底 + 白字"类别 置信度%"）
 * 输入参数：@left/@top - 检测框左上角
 *           @width     - 检测框宽度
 *           @name      - 类别名（如 "person"）
 *           @conf      - 置信度（0~100）
 * 注意事项：框太靠顶时标签改画框内，防止超出 canvas 边界
 *****************************************************************************/
static void ui_draw_box_label(int left, int top, int width,
                              const char* name, int conf)
{
    char label[UI_LABEL_TEXT_MAX];
    lv_draw_rect_dsc_t bg_dsc;
    lv_draw_label_dsc_t label_dsc;
    int label_y = (UI_LABEL_H <= top) ? (top - UI_LABEL_H) : top;

    /* 标签底色（半透明深灰） */
    lv_draw_rect_dsc_init(&bg_dsc);
    bg_dsc.bg_color = lv_color_make(0x20, 0x20, 0x20);
    bg_dsc.bg_opa = LV_OPA_70;
    lv_canvas_draw_rect(s_canvas, left, label_y, width, UI_LABEL_H, &bg_dsc);

    /* 标签文字（白色，超长由 max_width 截断） */
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_white();
    snprintf(label, sizeof(label), "%s %d%%", name, conf);
    lv_canvas_draw_text(s_canvas, left + 4, label_y + 2, width,
                        &label_dsc, label);
}

/*****************************************************************************
 * 函数名称：ui_draw_detect_boxes
 * 功能描述：在 canvas 上绘制检测框（绿色描边 + 类别标签）
 * 输入参数：@boxes - 检测框数组
 *           @count - 框数量
 *****************************************************************************/
static void ui_draw_detect_boxes(const detect_box_t* boxes, uint32_t count)
{
    lv_draw_rect_dsc_t dsc;
    uint32_t i;

    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = lv_color_make(0x00, 0xFF, 0x00);
    dsc.border_width = 2;

    for (i = 0; i < count; i++) {
        lv_canvas_draw_rect(s_canvas,
                            boxes[i].left, boxes[i].top,
                            boxes[i].right - boxes[i].left,
                            boxes[i].bottom - boxes[i].top,
                            &dsc);
        ui_draw_box_label(boxes[i].left, boxes[i].top,
                          boxes[i].right - boxes[i].left,
                          boxes[i].name, boxes[i].conf);
    }
}

/*****************************************************************************
 * 函数名称：ui_frame_tick
 * 功能描述：33ms 检查预览共享帧，新帧转 BGRA 上屏并叠加检测框
 * 输入参数：@timer - LVGL 定时器
 * 注意事项：预览独立于 AI——AI 关闭时预览照常显示
 *****************************************************************************/
static void ui_frame_tick(lv_timer_t* timer)
{
    ui_worker_t* ui = (ui_worker_t*)timer->user_data;
    detect_box_t boxes[DETECT_BOX_MAX];
    uint32_t box_count = 0;

    if ((NULL == ui) || (NULL == ui->preview_share) || (NULL == s_canvas_buf)) {
        return;
    }

    /* 有新帧 → BGRA 直拷 canvas（管线内已转换；先拷帧再画框避免旧框残影） */
    if (preview_share_pop(ui->preview_share, s_bgra_buf, &s_ui_frame_id)) {
        memcpy(s_canvas_buf, s_bgra_buf,
               sizeof(lv_color_t) * PREVIEW_WIDTH * PREVIEW_HEIGHT);

        /* AI 在线且画框开关开启时叠加最新检测框 */
        if (settings_get_ai_draw_box() && (NULL != ui->detect_share) &&
            detect_share_pop(ui->detect_share, boxes, &box_count)) {
            ui_draw_detect_boxes(boxes, box_count);
        }

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
 * 函数名称：ui_nav_to
 * 功能描述：跳转页面（平级导航，无返回栈；带动画加载目标页）
 * 输入参数：@target    - 目标页面
 *           @anim_type - 切换动画方向（进子页向左，回主页向右）
 * 注意事项：目标页已是当前页时跳过（防重复加载动画闪烁）
 *****************************************************************************/
static void ui_nav_to(lv_obj_t* target, lv_scr_load_anim_t anim_type)
{
    if (target == lv_scr_act()) {
        return;
    }
    lv_scr_load_anim(target, anim_type, 200, 0, false);
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
            /* 已在库页时跳过强制重建，避免整页闪烁
             * （每秒刷新定时器负责增量更新） */
            if (s_screen_library != lv_scr_act()) {
                ui_page_library_refresh();
            }
            ui_nav_to(s_screen_library, LV_SCR_LOAD_ANIM_MOVE_LEFT);
            break;
        case 1: /* 设置 */
            if (NULL == s_screen_settings) {
                s_screen_settings = ui_page_settings_create(ui);
            }
            ui_nav_to(s_screen_settings, LV_SCR_LOAD_ANIM_MOVE_LEFT);
            break;
        case 2: /* 主页（平级直达，无返回栈） */
            ui_nav_to(s_screen_live, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
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
 * 功能描述：创建右侧竖排导航栏（录像/设置/主页，各页面共用）
 * 输入参数：@parent - 父对象
 *           @ui     - UI 上下文（按钮回调）
 * 注意事项：全局导航组件，每个页面创建一份（LVGL 对象不可跨父级共享）
 *****************************************************************************/
void ui_nav_bar_create(lv_obj_t* parent, ui_worker_t* ui)
{
    static const char* const nav_texts[] = {"录像", "设置", "主页"};
    int btn_count = (int)(sizeof(nav_texts) / sizeof(nav_texts[0]));
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_t* btn;
    lv_obj_t* label;
    int i;

    /* 右侧导航栏：竖排平级导航（无返回键），
     * 按钮个数由文本表推导，增删条目同步生效 */
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

    /* 导航按钮（卡片灰底白字） */
    for (i = 0; i < btn_count; i++) {
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
    s_canvas_buf = (lv_color_t*)malloc(sizeof(lv_color_t) *
                                       PREVIEW_WIDTH * PREVIEW_HEIGHT);
    if (NULL == s_canvas_buf) {
        LOG_E("UI", "canvas 缓冲分配失败");
        return;
    }
    memset(s_canvas_buf, 0,
           sizeof(lv_color_t) * PREVIEW_WIDTH * PREVIEW_HEIGHT);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf,
                         PREVIEW_WIDTH, PREVIEW_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);

    /* 顶部状态栏（悬浮在预览上层） */
    ui_create_status_bar(screen);

    /* 右侧导航栏（悬浮在预览上层） */
    ui_nav_bar_create(screen, ui);

    /* 状态刷新（1s）+ 帧刷新（33ms） */
    lv_timer_create(ui_status_tick, 1000, ui);
    lv_timer_create(ui_frame_tick, 16, ui);
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
    LOG_I("UI", "线程启动（DRM 后端）");

    /* 触摸输入注册（Goodix 电容屏，失败不影响显示） */
    if (0 != touch_input_init()) {
        LOG_W("UI", "触摸输入不可用，界面仅作显示");
    }

    /* 中文字体初始化（freetype 渲染思源黑体，页面共享） */
    s_ft_info.name   = UI_FONT_PATH;
    s_ft_info.weight = 28;
    s_ft_info.style  = FT_FONT_STYLE_NORMAL;
    s_ft_info.mem    = NULL;
    if (!lv_ft_font_init(&s_ft_info)) {
        LOG_W("UI", "中文字体初始化失败，汉字将无法显示");
    }
    ui->font = s_ft_info.font;

    /* 缩略图后台生成管线（低优先级线程，失败不影响 UI） */
    if (0 != thumb_pipeline_init()) {
        LOG_W("UI", "缩略图管线初始化失败");
    }

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

    /* 停止缩略图生成线程 */
    thumb_pipeline_stop();

    /* 释放 canvas 缓冲 */
    if (NULL != s_canvas_buf) {
        free(s_canvas_buf);
        s_canvas_buf = NULL;
    }

    drm_disp_drv_deinit();
    LOG_I("UI", "线程退出");
    return NULL;
}
