#include "NV12con.h"
#include <cstring>
#include <cstddef>
#include <iostream>
#include <limits>

#include "im2d.h"
#include "RgaUtils.h"

YuvConverter::YuvConverter()
{
}

YuvConverter::~YuvConverter()
{
}

bool YuvConverter::convert(
    const VideoFrame& src,
    VideoFrame& dst)
{
    dst = VideoFrame{};
    dst.format = PIXEL_FORMAT_NV12;

    if (src.data == nullptr ||
        src.format != PIXEL_FORMAT_YUYV ||
        src.width <= 0 ||
        src.height <= 0 ||
        (src.width & 1) != 0 ||
        (src.height & 1) != 0)
    {
        std::cerr
            << "[RGA] invalid YUYV frame"
            << std::endl;
        return false;
    }

    const std::size_t width =
        static_cast<std::size_t>(src.width);

    const std::size_t height =
        static_cast<std::size_t>(src.height);

    const std::size_t stride =
        src.stride_bytes > 0
            ? static_cast<std::size_t>(src.stride_bytes)
            : width * 2U;

    // YUYV 每像素 2 字节。
    if (stride < width * 2U ||
        stride % 2U != 0)
    {
        std::cerr
            << "[RGA] invalid YUYV stride"
            << std::endl;
        return false;
    }

    const std::size_t required_input_size =
        stride * (height - 1U) + width * 2U;

    if (src.size <= 0 ||
        static_cast<std::size_t>(src.size) <
            required_input_size)
    {
        std::cerr
            << "[RGA] input buffer too small: got "
            << src.size
            << ", required "
            << required_input_size
            << std::endl;
        return false;
    }

    const std::size_t output_size =
        width * height * 3U / 2U;

    if (output_size >
        static_cast<std::size_t>(
            std::numeric_limits<int>::max()))
    {
        std::cerr
            << "[RGA] output buffer too large"
            << std::endl;
        return false;
    }

    // 同尺寸下复用缓冲区。
    buffer_.resize(output_size);

    // RGA 的行跨度单位为像素。
    // YUYV 每像素 2 字节，所以用 stride / 2。
    rga_buffer_t input = wrapbuffer_virtualaddr(
        src.data,
        src.width,
        src.height,
        RK_FORMAT_YUYV_422,
        static_cast<int>(stride / 2U),
        src.height
    );

    // 紧密排列的 NV12：Y 平面后接 UV 平面。
    rga_buffer_t output = wrapbuffer_virtualaddr(
        buffer_.data(),
        src.width,
        src.height,
        RK_FORMAT_YCbCr_420_SP,
        src.width,
        src.height
    );

    im_rect empty_rect = {};

    IM_STATUS status = imcheck(
        input,
        output,
        empty_rect,
        empty_rect
    );

    if (status != IM_STATUS_NOERROR)
    {
        std::cerr
            << "[RGA] check failed: "
            << imStrError(status)
            << std::endl;
        return false;
    }

    // sync = 1：等待硬件转换完成后返回。
    status = imcvtcolor(
        input,
        output,
        RK_FORMAT_YUYV_422,
        RK_FORMAT_YCbCr_420_SP,
        IM_COLOR_SPACE_DEFAULT,
        1
    );

    if (status != IM_STATUS_SUCCESS)
    {
        std::cerr
            << "[RGA] conversion failed: "
            << imStrError(status)
            << std::endl;
        return false;
    }

    dst.data = buffer_.data();
    dst.width = src.width;
    dst.height = src.height;
    dst.size = static_cast<int>(output_size);
    dst.format = PIXEL_FORMAT_NV12;
    dst.stride_bytes = src.width;
    dst.timestamp = src.timestamp;

    return true;
}

void YuvConverter::release(VideoFrame& frame)
{
    // 不执行 delete[]，buffer_ 在下一帧继续复用。
    frame = VideoFrame{};
    frame.format = PIXEL_FORMAT_NV12;
}