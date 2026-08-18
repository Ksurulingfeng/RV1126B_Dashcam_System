cmake_minimum_required(VERSION 3.12)  # CONFIGURE_DEPENDS 需 3.12+

set(TOOLCHAIN_DIR /opt/atk-dlrv1126b-toolchain)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-g++)
set(CMAKE_C_COMPILER ${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-gcc)
set(SYSROOT ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include)
set(CMAKE_SYSROOT ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot)

project(RV1126B_Dashcam_System LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# GStreamer（交叉编译：pkg-config 强制指向 sysroot，防止搜到宿主机库；
# 允许用户环境预设同名变量以扩展搜索路径）
if(NOT DEFINED ENV{PKG_CONFIG_LIBDIR})
    set(ENV{PKG_CONFIG_LIBDIR} "${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/lib/pkgconfig")
endif()
if(NOT DEFINED ENV{PKG_CONFIG_SYSROOT_DIR})
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0)
# RTSP 实时推流服务器（复用编码流 appsrc 桥接）
pkg_check_modules(RTSP_SERVER REQUIRED gstreamer-rtsp-server-1.0)
# FFmpeg（离线文件操作：缩略图/回放/导出/修复）
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.c" "src/*.cc")
# 测试代码双重排除：test 目录全部 + 散落各处的 *_test.c/cc
list(FILTER SOURCES EXCLUDE REGEX ".*/test/.*")
# 参考资料目录（lv_drivers 源码，RGA 专项备用）不参与编译
list(FILTER SOURCES EXCLUDE REGEX ".*/third_party/.*")
list(FILTER SOURCES EXCLUDE REGEX ".*_test\\.(c|cc)$")
# 独立播放器进程（src/player 有独立 main，单独编译成 dashcam_player）
list(FILTER SOURCES EXCLUDE REGEX ".*/player/.*")

# 录像回放独立进程源码：GStreamer 硬解 + KMS 全屏播放（UI 回放时拉起）
file(GLOB_RECURSE PLAYER_SOURCES CONFIGURE_DEPENDS "src/player/*.c")

set(OPENCV_LIBS opencv_core opencv_videoio opencv_video opencv_highgui opencv_imgcodecs opencv_imgproc)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/src/app
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/gps
    ${CMAKE_SOURCE_DIR}/src/ai
    ${CMAKE_SOURCE_DIR}/src/av
    ${CMAKE_SOURCE_DIR}/src/network
    ${CMAKE_SOURCE_DIR}/src/test
    ${CMAKE_SOURCE_DIR}/src/third_party/lv_drivers/display
    ${GST_INCLUDE_DIRS}
    ${RTSP_SERVER_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/rknn
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/rga
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/opencv4
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/lvgl/lv_drivers
)

target_compile_definitions(${PROJECT_NAME} PRIVATE LV_CONF_INCLUDE_SIMPLE)

target_link_libraries(${PROJECT_NAME} ${OPENCV_LIBS} pthread ${GST_LIBRARIES}
    ${RTSP_SERVER_LIBRARIES} ${FFMPEG_LIBRARIES} rknnrt rga lvgl lv_drivers drm)

# ===== 录像回放独立进程（LVGL 界面：canvas + 暂停/退出按钮） =====
# 复用主程序的触摸驱动（touch_input.c 无 UI 依赖，可独立编译）
add_executable(dashcam_player ${PLAYER_SOURCES} src/ui/touch_input.c)

target_include_directories(dashcam_player PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/src/third_party/lv_drivers/display
    ${GST_INCLUDE_DIRS}
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/lvgl
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/lvgl/lv_drivers
)

target_compile_definitions(dashcam_player PRIVATE LV_CONF_INCLUDE_SIMPLE)

# drm/freetype：lv_drivers 的 KMS 后端与 lvgl 字体引擎的依赖
# （主程序靠 OpenCV 间接带入，播放器无 OpenCV 需显式链接）
target_link_libraries(dashcam_player pthread ${GST_LIBRARIES} lvgl lv_drivers
    drm freetype)
