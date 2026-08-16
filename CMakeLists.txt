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
# FFmpeg（离线文件操作：缩略图/回放/导出/修复）
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.c" "src/*.cc")
# 测试代码双重排除：test 目录全部 + 散落各处的 *_test.c/cc
list(FILTER SOURCES EXCLUDE REGEX ".*/test/.*")
# 参考资料目录（lv_drivers 源码，RGA 专项备用）不参与编译
list(FILTER SOURCES EXCLUDE REGEX ".*/third_party/.*")
list(FILTER SOURCES EXCLUDE REGEX ".*_test\\.(c|cc)$")

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
    ${FFMPEG_INCLUDE_DIRS}
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/rknn
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/rga
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/opencv4
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include/lvgl/lv_drivers
)

target_compile_definitions(${PROJECT_NAME} PRIVATE LV_CONF_INCLUDE_SIMPLE)

target_link_libraries(${PROJECT_NAME} ${OPENCV_LIBS} pthread ${GST_LIBRARIES}
    ${FFMPEG_LIBRARIES} rknnrt rga lvgl lv_drivers)
