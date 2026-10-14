#ifndef VIDEO_FRAME_H
#define VIDEO_FRAME_H

#include <stdint.h>

enum PixelFormat
{
    PIXEL_FORMAT_YUYV,
    PIXEL_FORMAT_NV12,
    PIXEL_FORMAT_MJPEG
};

struct VideoFrame
{
    uint8_t* data;

    int width;
    int height;
    int size;

    int format;

    // 每行实际占用的字节数，包含行尾填充。
    int stride_bytes;

    uint64_t timestamp;
};

#endif