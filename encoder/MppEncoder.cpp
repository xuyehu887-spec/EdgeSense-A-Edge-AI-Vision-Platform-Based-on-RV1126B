#include "MppEncoder.h"

#include <cstring>
#include <iostream>
#include <limits>

const std::size_t MppEncoder::DEFAULT_OUTPUT_CAPACITY;

namespace
{

int align16(int value)
{
    return (value + 15) & ~15;
}

/*
 * 将紧密排列的 NV12 数据复制到带 MPP stride 的输入缓冲区。
 */
void copyNV12WithStride(
    uint8_t* dst,
    int dst_hor_stride,
    int dst_ver_stride,
    const uint8_t* src,
    int width,
    int height
)
{
    const std::size_t dst_size =
        static_cast<std::size_t>(dst_hor_stride) *
        static_cast<std::size_t>(dst_ver_stride) *
        3U / 2U;

    std::memset(dst, 0, dst_size);

    /*
     * 复制 Y 平面。
     */
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* src_row =
            src +
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(width);

        uint8_t* dst_row =
            dst +
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(dst_hor_stride);

        std::memcpy(
            dst_row,
            src_row,
            static_cast<std::size_t>(width)
        );
    }

    /*
     * 复制 UV 平面。
     */
    const uint8_t* src_uv =
        src +
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);

    uint8_t* dst_uv =
        dst +
        static_cast<std::size_t>(dst_hor_stride) *
        static_cast<std::size_t>(dst_ver_stride);

    for (int y = 0; y < height / 2; ++y)
    {
        const uint8_t* src_row =
            src_uv +
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(width);

        uint8_t* dst_row =
            dst_uv +
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(dst_hor_stride);

        std::memcpy(
            dst_row,
            src_row,
            static_cast<std::size_t>(width)
        );
    }
}

int clampToInt(uint64_t value)
{
    const uint64_t maximum =
        static_cast<uint64_t>(
            std::numeric_limits<int>::max()
        );

    return static_cast<int>(
        value > maximum ? maximum : value
    );
}

} // namespace

MppEncoder::MppEncoder()
    : ctx_(nullptr),
      mpi_(nullptr),
      cfg_(nullptr),
      buffer_group_(nullptr),
      input_buffer_(nullptr),
      width_(0),
           height_(0),
      fps_(30),
      bitrate_(0),
      raw_frame_size_(0),
      input_buffer_size_(0),
      initialized_(false),
      frame_count_(0)
{
}

MppEncoder::~MppEncoder()
{
    release();
}

bool MppEncoder::init(
    int width,
    int height,
    int fps,
    int bitrate
)
{
    release();

    if (width <= 0 ||
        height <= 0 ||
        fps <= 0)
    {
        std::cerr
            << "[MPP] invalid width, height or fps"
            << std::endl;

        return false;
    }

    /*
     * NV12 的宽和高必须是偶数。
     */
    if ((width & 1) != 0 ||
        (height & 1) != 0)
    {
        std::cerr
            << "[MPP] NV12 width and height must be even"
            << std::endl;

        return false;
    }

    width_ = width;
    height_ = height;
    fps_ = fps;

    if (bitrate > 0)
    {
        bitrate_ = bitrate;
    }
    else
    {
        /*
         * 640x480 时约为 2.46 Mbps。
         */
        const uint64_t calculated =
            static_cast<uint64_t>(width_) *
            static_cast<uint64_t>(height_) *
            8ULL;

        bitrate_ = clampToInt(calculated);
    }

    raw_frame_size_ =
        static_cast<std::size_t>(width_) *
        static_cast<std::size_t>(height_) *
        3U / 2U;

    const int hor_stride = align16(width_);
    const int ver_stride = align16(height_);

    input_buffer_size_ =
        static_cast<std::size_t>(hor_stride) *
        static_cast<std::size_t>(ver_stride) *
        3U / 2U;

    MPP_RET ret =
        mpp_create(
            &ctx_,
            &mpi_
        );

    if (ret != MPP_OK ||
        ctx_ == nullptr ||
        mpi_ == nullptr)
    {
        std::cerr
            << "[MPP] mpp_create failed, ret="
            << ret
            << std::endl;

        release();
        return false;
    }

    /*
     * 输出使用阻塞模式，保证取到当前编码结果。
     */
    MppPollType timeout =
        MPP_POLL_NON_BLOCK;

    ret = mpi_->control(
        ctx_,
        MPP_SET_OUTPUT_TIMEOUT,
        &timeout
    );

    if (ret != MPP_OK)
    {
        std::cerr
            << "[MPP] warning: set output timeout failed, ret="
            << ret
            << std::endl;
    }

    ret = mpp_init(
        ctx_,
        MPP_CTX_ENC,
        MPP_VIDEO_CodingAVC
    );

    if (ret != MPP_OK)
    {
        std::cerr
            << "[MPP] mpp_init failed, ret="
            << ret
            << std::endl;

        release();
        return false;
    }

    /*
     * RV1126B 使用 ION 输入缓冲区。
     */
    ret = mpp_buffer_group_get_internal(
        &buffer_group_,
        MPP_BUFFER_TYPE_ION
    );

    if (ret != MPP_OK ||
        buffer_group_ == nullptr)
    {
        std::cerr
            << "[MPP] create buffer group failed, ret="
            << ret
            << std::endl;

        release();
        return false;
    }

    /*
     * 输入缓冲区只申请一次，每一帧重复使用。
     */
    ret = mpp_buffer_get(
        buffer_group_,
        &input_buffer_,
        input_buffer_size_
    );

    if (ret != MPP_OK ||
        input_buffer_ == nullptr)
    {
        std::cerr
            << "[MPP] allocate input buffer failed, ret="
            << ret
            << std::endl;

        release();
        return false;
    }

    if (!setup())
    {
        release();
        return false;
    }

    frame_count_ = 0;
    initialized_ = true;

    std::cout
        << "[MPP] H264 encoder initialized: "
        << width_
        << "x"
        << height_
        << " @ "
        << fps_
        << " FPS, bitrate="
        << bitrate_
        << std::endl;

    return true;
}

bool MppEncoder::setConfigS32(
    const char* name,
    int value,
    bool required
)
{
    const MPP_RET ret =
        mpp_enc_cfg_set_s32(
            cfg_,
            name,
            value
        );

    if (ret == MPP_OK)
    {
        return true;
    }

    std::cerr
        << "[MPP] "
        << (required ? "required" : "optional")
        << " config failed: "
        << name
        << "="
        << value
        << ", ret="
        << ret
        << std::endl;

    /*
     * 可选配置失败时继续运行。
     */
    return !required;
}

bool MppEncoder::setup()
{
    MPP_RET ret =
        mpp_enc_cfg_init(
            &cfg_
        );

    if (ret != MPP_OK ||
        cfg_ == nullptr)
    {
        std::cerr
            << "[MPP] mpp_enc_cfg_init failed, ret="
            << ret
            << std::endl;

        return false;
    }

    ret = mpi_->control(
        ctx_,
        MPP_ENC_GET_CFG,
        cfg_
    );

    if (ret != MPP_OK)
    {
        std::cerr
            << "[MPP] MPP_ENC_GET_CFG failed, ret="
            << ret
            << std::endl;

        return false;
    }

    const int hor_stride =
        align16(width_);

    const int ver_stride =
        align16(height_);

    bool ok = true;

    ok =
        setConfigS32(
            "prep:width",
            width_,
            true
        ) && ok;

    ok =
        setConfigS32(
            "prep:height",
            height_,
            true
        ) && ok;

    ok =
        setConfigS32(
            "prep:hor_stride",
            hor_stride,
            true
        ) && ok;

    ok =
        setConfigS32(
            "prep:ver_stride",
            ver_stride,
            true
        ) && ok;

    ok =
        setConfigS32(
            "prep:format",
            MPP_FMT_YUV420SP,
            true
        ) && ok;

    /*
     * CBR 码率控制。
     */
    ok =
        setConfigS32(
            "rc:mode",
            MPP_ENC_RC_MODE_CBR,
            true
        ) && ok;

    ok =
        setConfigS32(
            "rc:bps_target",
            bitrate_,
            true
        ) && ok;

    const int bps_max =
        clampToInt(
            static_cast<uint64_t>(bitrate_) *
            17ULL / 16ULL
        );

    const int bps_min =
        clampToInt(
            static_cast<uint64_t>(bitrate_) *
            15ULL / 16ULL
        );

    ok =
        setConfigS32(
            "rc:bps_max",
            bps_max,
            true
        ) && ok;

    ok =
        setConfigS32(
            "rc:bps_min",
            bps_min,
            true
        ) && ok;

    /*
     * 固定输入和输出帧率。
     */
    setConfigS32(
        "rc:fps_in_flex",
        0,
        false
    );

    setConfigS32(
        "rc:fps_in_num",
        fps_,
        false
    );

    /*
     * 新版使用 denom，部分旧版使用 denorm。
     */
    ret = mpp_enc_cfg_set_s32(
        cfg_,
        "rc:fps_in_denom",
        1
    );

    if (ret != MPP_OK)
    {
        ret = mpp_enc_cfg_set_s32(
            cfg_,
            "rc:fps_in_denorm",
            1
        );
    }

    if (ret != MPP_OK)
    {
        std::cerr
            << "[MPP] warning: input FPS denominator unsupported"
            << std::endl;
    }

    setConfigS32(
        "rc:fps_out_flex",
        0,
        false
    );

    setConfigS32(
        "rc:fps_out_num",
        fps_,
        false
    );

    ret = mpp_enc_cfg_set_s32(
        cfg_,
        "rc:fps_out_denom",
        1
    );

    if (ret != MPP_OK)
    {
        ret = mpp_enc_cfg_set_s32(
            cfg_,
            "rc:fps_out_denorm",
            1
        );
    }

    if (ret != MPP_OK)
    {
        std::cerr
            << "[MPP] warning: output FPS denominator unsupported"
            << std::endl;
    }

    /*
     * 每秒产生一个 IDR。
     */
    ok =
        setConfigS32(
            "rc:gop",
            fps_,
            true
        ) && ok;


/*
 * 编码跟不上时丢帧，避免延迟累积
 */
    ok =
        setConfigS32(
            "rc:drop_mode",
            1,
            false
        ) && ok;
        

    /*
     * H.264 Baseline Profile。
     * 这些参数在不同 MPP 版本间可能不同，因此设为可选。
     */
    setConfigS32(
        "h264:profile",
        66,
        false
    );

    setConfigS32(
        "h264:level",
        30,
        false
    );

    setConfigS32(
        "h264:cabac_en",
        0,
        false
    );

    setConfigS32(
        "h264:cabac_idc",
        0,
        false
    );

    setConfigS32(
        "h264:trans8x8",
        0,
        false
    );

    if (!ok)
    {
        return false;
    }

    ret = mpi_->control(
        ctx_,
        MPP_ENC_SET_CFG,
        cfg_
    );

    if (ret != MPP_OK)
    {
        std::cerr
            << "[MPP] MPP_ENC_SET_CFG failed, ret="
            << ret
            << std::endl;

        return false;
    }

    /*
     * 每个 IDR 携带 SPS/PPS，方便 RTSP 客户端中途连接。
     */
    MppEncHeaderMode header_mode =
        MPP_ENC_HEADER_MODE_EACH_IDR;

    ret = mpi_->control(
        ctx_,
        MPP_ENC_SET_HEADER_MODE,
        &header_mode
    );

    if (ret != MPP_OK)
    {
        std::cerr
            << "[MPP] warning: set header mode failed, ret="
            << ret
            << std::endl;
    }

    return true;
}

bool MppEncoder::encode(
    VideoFrame& frame,
    uint8_t* output,
    int& output_size
)
{
    return encode(
        static_cast<const VideoFrame&>(frame),
        output,
        DEFAULT_OUTPUT_CAPACITY,
        output_size
    );
}

bool MppEncoder::encode(
    const VideoFrame& frame,
    uint8_t* output,
    std::size_t output_capacity,
    int& output_size
)
{
    output_size = 0;

    if (!initialized_ ||
        ctx_ == nullptr ||
        mpi_ == nullptr ||
        input_buffer_ == nullptr)
    {
        std::cerr
            << "[MPP] encoder is not initialized"
            << std::endl;

        return false;
    }

    if (frame.data == nullptr ||
        output == nullptr ||
        output_capacity == 0)
    {
        std::cerr
            << "[MPP] null input/output buffer"
            << std::endl;

        return false;
    }

    if (frame.format != PIXEL_FORMAT_NV12)
    {
        std::cerr
            << "[MPP] input frame is not NV12"
            << std::endl;

        return false;
    }

    if (frame.width != width_ ||
        frame.height != height_)
    {
        std::cerr
            << "[MPP] frame size mismatch: got "
            << frame.width
            << "x"
            << frame.height
            << ", expected "
            << width_
            << "x"
            << height_
            << std::endl;

        return false;
    }

    if (frame.size <= 0 ||
        static_cast<std::size_t>(frame.size) <
            raw_frame_size_)
    {
        std::cerr
            << "[MPP] NV12 frame too small: got "
            << frame.size
            << ", expected at least "
            << raw_frame_size_
            << std::endl;

        return false;
    }

    uint8_t* input_ptr =
        static_cast<uint8_t*>(
            mpp_buffer_get_ptr(
                input_buffer_
            )
        );

    if (input_ptr == nullptr)
    {
        std::cerr
            << "[MPP] input buffer has no CPU address"
            << std::endl;

        return false;
    }

    const int hor_stride =
        align16(width_);

    const int ver_stride =
        align16(height_);

    /*
     * RV1126B 这套 release MPP 没有
     * mpp_buffer_sync_begin/end。
     * ION 输入缓冲区直接写入。
     */
    copyNV12WithStride(
        input_ptr,
        hor_stride,
        ver_stride,
        frame.data,
        width_,
        height_
    );

    MppFrame mpp_frame =
        nullptr;

    MPP_RET ret =
        mpp_frame_init(
            &mpp_frame
        );

    if (ret != MPP_OK ||
        mpp_frame == nullptr)
    {
        std::cerr
            << "[MPP] mpp_frame_init failed, ret="
            << ret
            << std::endl;

        return false;
    }

    mpp_frame_set_width(
        mpp_frame,
        width_
    );

    mpp_frame_set_height(
        mpp_frame,
        height_
    );

    mpp_frame_set_hor_stride(
        mpp_frame,
        hor_stride
    );

    mpp_frame_set_ver_stride(
        mpp_frame,
        ver_stride
    );

    mpp_frame_set_fmt(
        mpp_frame,
        MPP_FMT_YUV420SP
    );

    mpp_frame_set_eos(
        mpp_frame,
        0
    );

    mpp_frame_set_buffer(
        mpp_frame,
        input_buffer_
    );

    ret = mpi_->encode_put_frame(
        ctx_,
        mpp_frame
    );

    mpp_frame_deinit(
        &mpp_frame
    );

    if (ret != MPP_OK)
    {
        std::cerr
            << "[MPP] encode_put_frame failed, ret="
            << ret
            << std::endl;

        return false;
    }

    MppPacket packet =
        nullptr;

    ret = mpi_->encode_get_packet(
        ctx_,
        &packet
    );

    if (ret != MPP_OK ||
        packet == nullptr)
    {
        std::cerr
            << "[MPP] encode_get_packet failed, ret="
            << ret
            << std::endl;

        if (packet != nullptr)
        {
            mpp_packet_deinit(
                &packet
            );
        }

        return false;
    }

    const uint8_t* packet_data =
        static_cast<const uint8_t*>(
            mpp_packet_get_pos(
                packet
            )
        );

    const std::size_t packet_size =
        mpp_packet_get_length(
            packet
        );

    if (packet_data == nullptr ||
        packet_size == 0)
    {
        std::cerr
            << "[MPP] encoder returned empty packet"
            << std::endl;

        mpp_packet_deinit(
            &packet
        );

        return false;
    }

    if (packet_size > output_capacity ||
        packet_size >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            ))
    {
        std::cerr
            << "[MPP] output buffer too small: packet="
            << packet_size
            << ", capacity="
            << output_capacity
            << std::endl;

        mpp_packet_deinit(
            &packet
        );

        return false;
    }

    /*
     * 只复制当前编码包。
     * 不拼接以前保存的 IDR 或 SPS/PPS 数据。
     */
    std::memcpy(
        output,
        packet_data,
        packet_size
    );

    output_size =
        static_cast<int>(
            packet_size
        );

    mpp_packet_deinit(
        &packet
    );

    ++frame_count_;

    return true;
}

void MppEncoder::release()
{
    initialized_ = false;

    if (ctx_ != nullptr)
    {
        if (mpi_ != nullptr)
        {
            mpi_->reset(
                ctx_
            );
        }

        mpp_destroy(
            ctx_
        );

        ctx_ = nullptr;
    }

    mpi_ = nullptr;

    if (cfg_ != nullptr)
    {
        mpp_enc_cfg_deinit(
            cfg_
        );

        cfg_ = nullptr;
    }

    if (input_buffer_ != nullptr)
    {
        mpp_buffer_put(
            input_buffer_
        );

        input_buffer_ = nullptr;
    }

    if (buffer_group_ != nullptr)
    {
        mpp_buffer_group_put(
            buffer_group_
        );

        buffer_group_ = nullptr;
    }

    width_ = 0;
    height_ = 0;
    fps_ = 30;
    bitrate_ = 0;

    raw_frame_size_ = 0;
    input_buffer_size_ = 0;

    frame_count_ = 0;
}

bool MppEncoder::isInitialized() const
{
    return initialized_;
}