#ifndef NV12_CONVERTER_H
#define NV12_CONVERTER_H

#include <cstdint>
#include <vector>

#include "../video/VideoFrame.h"

class YuvConverter
{
public:
    YuvConverter();
    ~YuvConverter();

    bool convert(
        const VideoFrame& src,
        VideoFrame& dst
    );

    // 仅清空帧引用，内存由 buffer_ 管理。
    void release(VideoFrame& frame);

private:
    std::vector<uint8_t> buffer_;
};

#endif