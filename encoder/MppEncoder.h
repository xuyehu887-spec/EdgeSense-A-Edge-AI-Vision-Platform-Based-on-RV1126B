#ifndef MPP_ENCODER_H
#define MPP_ENCODER_H

#include <cstddef>
#include <cstdint>

#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>

#include "../video/VideoFrame.h"

class MppEncoder
{
public:
    static const std::size_t DEFAULT_OUTPUT_CAPACITY =
        1024U * 1024U;

    MppEncoder();
    ~MppEncoder();

    bool init(
        int width,
        int height,
        int fps = 30,
        int bitrate = 0
    );

    bool encode(
        VideoFrame& frame,
        uint8_t* output,
        int& output_size
    );

    bool encode(
        const VideoFrame& frame,
        uint8_t* output,
        std::size_t output_capacity,
        int& output_size
    );

    void release();

    bool isInitialized() const;

private:
    bool setup();

    bool setConfigS32(
        const char* name,
        int value,
        bool required
    );

private:
    MppCtx ctx_;
    MppApi* mpi_;
    MppEncCfg cfg_;

    MppBufferGroup buffer_group_;
    MppBuffer input_buffer_;

    int width_;
    int height_;
    int fps_;
    int bitrate_;

    std::size_t raw_frame_size_;
    std::size_t input_buffer_size_;

    bool initialized_;
    uint64_t frame_count_;
};

#endif