cmake_minimum_required(VERSION 3.10)

project(xm1)


# ============================================================
# 1. 交叉编译工具链
# ============================================================

set(TOOLCHAIN_DIR
    /opt/atk-dlrv1126b-toolchain
)

set(CMAKE_C_COMPILER
    ${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-gcc
)

set(CMAKE_CXX_COMPILER
    ${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-g++
)

set(CMAKE_SYSROOT
    ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot
)


# ============================================================
# 2. C++标准
# ============================================================

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# ============================================================
# 3. Rockchip MPP
# ============================================================

set(MPP_DIR
    /home/a/software/github-rknn/rknn-toolkit2-master/rknpu2/examples/3rdparty/mpp
)

set(GLIB_CONFIG_INCLUDE_DIR
    ${CMAKE_SYSROOT}/usr/lib/glib-2.0/include
)
# ============================================================
# 4. GStreamer
# ============================================================

set(GSTREAMER_INCLUDE_DIR
    ${CMAKE_SYSROOT}/usr/include/gstreamer-1.0
)

set(GLIB_INCLUDE_DIR
    ${CMAKE_SYSROOT}/usr/include/glib-2.0
)


# ============================================================
# 5. Include目录
# ============================================================

include_directories(

    ${CMAKE_SYSROOT}/usr/include

    ${GSTREAMER_INCLUDE_DIR}

    ${GLIB_INCLUDE_DIR}

    ${GLIB_CONFIG_INCLUDE_DIR}

    camera

    video

    encoder

    rtsp

    ${MPP_DIR}/include

      /opt/atk-dlrv1126b-toolchain/aarch64-buildroot-linux-gnu/sysroot/usr/include/rockchip
)


# ============================================================
# 6. 源文件
# ============================================================

add_executable(
    smartvision

    app/main.cpp
    camera/CameraManager.cpp
    encoder/MppEncoder.cpp
    encoder/NV12con.cpp
    rtsp/RtspServer.cpp

    inference/YoloV8Detector.cpp
    inference/DrawNV12.cpp
)

target_include_directories(smartvision PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/inference"
    "${CMAKE_SYSROOT}/usr/include/rga"
    "${CMAKE_SYSROOT}/usr/include/rknn"
)

# 链接工具链中的 ARM64 RGA 库
# ARM64 RGA 和 RKNN 运行库
target_link_libraries(smartvision
    "${CMAKE_SYSROOT}/usr/lib/librga.so"
    "${CMAKE_SYSROOT}/usr/lib/librknnrt.so"
)
# ============================================================
# 7. Library目录
# ============================================================

link_directories(

    ${CMAKE_SYSROOT}/usr/lib

    ${MPP_DIR}/lib
)


# ============================================================
# 8. 链接库
# ============================================================


target_link_libraries(
    smartvision

    pthread

    rockchip_mpp

    gstreamer-1.0
    gstapp-1.0
    gstrtspserver-1.0

    gstbase-1.0
    gstnet-1.0

    gobject-2.0
    glib-2.0
)