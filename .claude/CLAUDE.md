<!-- zh-helper:start -->

## 语言设置

- **始终使用简体中文回复。** 所有解释、摘要、注释和生成的文档必须使用中文。
- 代码标识符（变量名、函数名、类名）保持英文原样。
- 代码注释：解释性注释使用中文；行内类型注解和 docstring 标签（如 `@param`、`@returns`）保持英文。
- Git commit message：使用中文，格式为 `<type>: <中文描述>`（例：`fix: 修复登录超时问题`）。
- 引用外部资源（URL、库名、CLI 命令）时保持原语言。
- 生成的 Markdown 文档正文使用中文，章节标题使用中文。

<!-- zh-helper:end -->

<!-- huawei-c:start -->

## C 语言编码规范（华为 2011 版）

**作用域**：本规范**仅适用于 C/C++ 文件**（`.c`、`.h`、`.cpp`、`.hpp`），其他文件类型不受此约束。例外：`src/third_party/` 为上游参考源码（lv_drivers 等，不参与编译），原样保留便于对照，不强制本规范。

本项目的所有 C/C++ 代码**必须严格遵循**以下规则。此规范优先级高于其他编码建议。

### 排版与格式

- **缩进**：4 个空格，禁用 Tab。每行 ≤ 80 字符，续行缩进至少 8 个空格。
- **函数大括号**：左大括号另起一行，独占一行。
- **控制语句大括号**：`if`/`for`/`while`/`do`/`switch` 的左大括号跟在表达式后面，不换行。即使单条语句也必须用 `{ }` 括起来。
- **空格**：关键字后加一空格再跟括号；二元运算符前后各加一空格；逗号/分号后加空格；函数名与括号之间不加空格。
- **空行**：函数之间两个空行；函数内逻辑段落之间一个空行；变量声明与执行语句之间一个空行。

### 命名规范

| 类型 | 格式 | 示例 |
|------|------|------|
| 变量/函数 | `snake_case` | `temp_value`, `get_user_info()` |
| 全局变量 | `g_` 前缀 | `g_total_count` |
| 静态全局变量 | `s_` 前缀 | `s_state_machine` |
| 布尔变量 | `is_`/`has_`/`can_` 前缀 | `is_valid` |
| 宏 | `UPPER_SNAKE_CASE` | `MAX_BUFFER_SIZE` |
| typedef | `_t` 后缀 | `device_info_t` |
| 枚举常量 | 统一前缀的大写 | `COLOR_RED` |
| 文件名 | `snake_case` | `uart_driver.c` |

### 注释规范

- **文件头**：每个 `.c`/`.h` 必须有版权声明、功能描述、作者、日期。
- **函数头**：每个非静态函数前必须有功能、参数（`@param`）、返回值、注意事项注释。
- **行内注释**：复杂逻辑必须注释，使用 `/* */` 风格。
- **注释量**：有效注释不低于源码总行数 20%。

### 头文件

- `#ifndef`/`#define` 防重复包含，宏名格式 `FILENAME_H`。
- 职责单一，禁止循环依赖。
- 包含顺序：系统头文件 → 项目头文件。

### 函数设计

- 单一职责，≤ 50 行（不含注释/空行），参数 ≤ 5 个。
- 返回 `0` 表示成功，负数表示错误。
- 避免全局变量，优先参数传递。

### 内存与资源管理

- `malloc`/`calloc` 后必须检查 `NULL`；`free` 后置 `NULL`。
- 每个 `malloc` 有对应 `free`，每个 `fopen` 有对应 `fclose`。
- 用 `goto` 集中错误释放（华为规范允许）。

### 其他规则

- 禁用 `void *` 指针运算；`switch` 必须有 `default` 分支。
- 禁用 `continue` 和非错误处理的 `goto`。
- 魔法数字必须定义为宏/枚举常量。
- 常量放比较表达式左侧：`if (0 == ret)`。
- 优先用清晰的命名代替过多注释。

<!-- huawei-c:end -->

<!-- project-guide:start -->

## 常用命令

### 交叉编译（RV1126B 交叉工具链）
```bash
./scripts/build.sh          # Release 编译（CMake + Ninja，产物 build/RV1126B_Dashcam_System、build/dashcam_player）
./scripts/build.sh -d       # Debug
./scripts/build.sh -c       # 清理后重编
```

### 部署到开发板（adb 连接 ATK-DLRV1126B）
```bash
./scripts/deploy.sh -n -r   # 部署并重启程序（-i 安装开机自启 S50dashcam）
```
板端程序目录 `/root/RV1126B_Dashcam_System`，录像目录 `/mnt/sdcard/videos`。

### PC 单元测试（src/test/ 不参与 CMake，手动 gcc）
```bash
# 例：video_recover 恢复测试（PC 可跑，无需开发板）
gcc -o /tmp/test_recover -I src/common -I src/av \
    src/test/test_video_recover.c src/av/video_recover.c \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil)
```
`test_file_mgr.c`、`test_nmea.c`、`test_thumb_gen.c` 同法（对应 src/core、src/gps、src/av）。

## 架构概览

### 两个可执行文件
| 目标 | 源码 | 职责 |
|------|------|------|
| `RV1126B_Dashcam_System` | [src/app/main.c](src/app/main.c) + 各模块 | 主程序：录像/断电恢复/UI/AI/GPS/推流 |
| `dashcam_player` | [src/player/player_main.c](src/player/player_main.c) + [src/ui/touch_input.c](src/ui/touch_input.c) | 独立回放播放器（LVGL 界面，UI fork 拉起） |

### GStreamer 实时管线（[src/av/gst_encoder.c](src/av/gst_encoder.c)）
`parse-launch` 字符串构建（板端实测**手写 API 会致 mux 不输出 0 字节，勿改**）：
```
v4l2src → capsfilter → tee t
  t → queue → mpph264enc → tee te
     te → valve rv → h264parse → splitmuxsink（录像分支）
     te → queue → h264parse → valve sv → rtph264pay → udpsink（推流分支）
  t → queue → videoscale → tee t2 → appsink nv12/bgra（预览分支）
```
- `valve` 是录像/推流的运行时开关（`drop` 属性），UI 改 settings → 主循环巡检 ≤1s 应用
- 推流 RTP 用 `rtph264pay config-interval=-1` 周期插 SPS/PPS；**禁止**动编码器 `header-mode` 或 h264parse `config-interval`（独立 SPS/PPS 样本会让 qtmux/mpegtsmux dts 非单调卡死）
- 播放器/推流踩坑详见记忆 `rtsp-streaming-pitfalls` 与 `fmp4-fragment-unsolved`

### 设置-巡检模式（[src/common/settings.c](src/common/settings.c) + main.c）
- UI 只写 settings（key=value 持久化 + 互斥锁），主循环每秒巡检变化后调 gst_encoder 接口
- 启动期生效项（录音/推流分支构建）在 `gst_encoder_config_t`；运行期项（录像/推流/分段时长）走 valve/max-size-time

### 断电恢复（[src/av/video_recover.c](src/av/video_recover.c)）
- 启动时扫描断电残留（mdat 无 moov），AVCC 长度前缀格式零拷贝重封装 + 借用同目录完好文件 avcC + 连续两帧验证过滤伪命中

### LVGL UI（[src/ui/](src/ui/)）
- [ui_main.c](src/ui/ui_main.c)：LVGL + DRM 显示（`drm_disp_drv_init` 旋转 90° 逻辑横屏 1280×720）、`lv_timer_handler` 主循环
- [ui_pages.c](src/ui/ui_pages.c)：文件库（缩略图后台管线）/设置页/点击行拉起播放器（fork + waitpid，期间 LVGL 停刷避免抢屏，结束后 `drmSetMaster` 恢复显示）
- [touch_input.c](src/ui/touch_input.c)：自写 /dev/input/event1 MT 读取（O_NONBLOCK + read_cb，非 LVGL 官方 evdev）

### 关键约定
- 代码注释/文档用简体中文；commit `<type>: 中文描述`；作者署名 heifast
- `docs/` 在 .gitignore（本地归档不入库）；README.md 入 git
- 华为 C 规范仅约束 .c/.h；`src/third_party/`（lv_drivers 参考源码）豁免
- 板端坑（详见记忆）：SIGKILL 丢缓冲日志（LOG 已改 stderr）；pkill 按 15 字符截断匹配失败；adb shell 后台启动用 `setsid`；ffplay 收自己发的广播不回环

<!-- project-guide:end -->
