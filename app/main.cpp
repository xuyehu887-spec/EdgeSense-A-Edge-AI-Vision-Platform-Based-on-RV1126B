#include <chrono>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#include "YoloV8Detector.h"
#include "DrawNV12.h"
#include "AsyncPersonDetector.h"

#include "CameraManager.h"
#include "MppEncoder.h"
#include "NV12con.h"
#include "RtspServer.h"
#include "VideoFrame.h"

namespace
{
volatile std::sig_atomic_t g_running = 1;

void signalHandler(int)
{
    g_running = 0;
}
}

int main(int argc, char** argv)
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 参数1：摄像头设备；参数2：模型路径。
    const char* device = "/dev/video52";
    const char* modelPath = "./model/yolov8n_person.rknn";

    if (argc > 1)
    {
        device = argv[1];
    }

    if (argc > 2)
    {
        modelPath = argv[2];
    }

    const int width = 640;
    const int height = 480;
    const int fps = 30;
    const int rtspPort = 8554;

    std::cout
        << "====================================\n"
        << " RV1126B RTSP + ASYNC PERSON DETECTION\n"
        << "====================================\n"
        << "Device : " << device << '\n'
        << "Model  : " << modelPath << '\n'
        << "Format : YUYV\n"
        << "Size   : " << width << 'x' << height << '\n'
        << "FPS    : " << fps
        << std::endl;

    // 模型在主线程初始化一次。
    // 启动异步线程后，只有检测线程可以调用 detector.detect()。
    YoloV8Detector detector;

    if (!detector.init(modelPath))
    {
        std::cerr
            << "[RKNN] model init failed: "
            << modelPath
            << std::endl;

        return -1;
    }

    CameraManager camera;

    if (!camera.open(device, width, height, fps))
    {
        std::cerr << "[Camera] open failed" << std::endl;
        return -1;
    }

    if (!camera.start())
    {
        std::cerr << "[Camera] start failed" << std::endl;
        return -1;
    }

    std::cout << "[Camera] OK" << std::endl;

    // 保留你已经调好的 RGA YUYV→NV12 转换。
    YuvConverter converter;

    MppEncoder encoder;

    if (!encoder.init(width, height, fps))
    {
        std::cerr << "[MPP] init failed" << std::endl;
        camera.stop();
        return -1;
    }

    std::cout << "[MPP] OK" << std::endl;

    RtspServer rtsp;

    if (!rtsp.start(rtspPort))
    {
        std::cerr << "[RTSP] start failed" << std::endl;
        encoder.release();
        camera.stop();
        return -1;
    }

    std::cout
        << "[RTSP] rtsp://<BOARD_IP>:"
        << rtspPort
        << "/live\n"
        << "[RTSP] server ready; client can connect at any time"
        << std::endl;

    std::vector<uint8_t> h264Buffer(
        MppEncoder::DEFAULT_OUTPUT_CAPACITY
    );

    // 主线程读取的最近一次检测结果。
    std::vector<Detection> detections;

    uint64_t debugCount = 0;
    uint64_t totalFrames = 0;
    uint64_t encodedFrames = 0;
    uint64_t rtspFrames = 0;

    using Clock = std::chrono::steady_clock;

    // 必须定义在 detector 之后，确保先停止线程再销毁模型。
    AsyncPersonDetector asyncDetector(detector);
    asyncDetector.start();

    Clock::time_point lastPrint = Clock::now();

    std::cout << "Start streaming..." << std::endl;

    while (g_running != 0)
    {
        ++debugCount;

        // 1. 从摄像头获取 YUYV。
        const auto capture_t0 = Clock::now();

        VideoFrame yuyv{};

        if (!camera.getFrame(yuyv))
        {
            std::cerr << "[Camera] get failed" << std::endl;
            break;
        }

        // 归还摄像头缓冲区前保存时间戳。
        const uint64_t ptsUs = yuyv.timestamp;

        const auto capture_t1 = Clock::now();

        if (debugCount % 30 == 0)
        {
            const double capture_ms =
                std::chrono::duration<double, std::milli>(
                    capture_t1 - capture_t0
                ).count();

            std::cout
                << "[TIME] capture="
                << capture_ms
                << " ms timestamp="
                << ptsUs
                << std::endl;
        }

        ++totalFrames;

        // 2. 使用现有 RGA 转换为紧密排列的 NV12。
        VideoFrame nv12{};

        const auto convert_t0 = Clock::now();

        if (!converter.convert(yuyv, nv12))
        {
            std::cerr << "[NV12] convert failed" << std::endl;

            camera.releaseFrame();
            continue;
        }

        const auto convert_t1 = Clock::now();

        if (debugCount % 30 == 0)
        {
            const double convert_ms =
                std::chrono::duration<double, std::milli>(
                    convert_t1 - convert_t0
                ).count();

            std::cout
                << "[TIME] YUYV->NV12="
                << convert_ms
                << " ms"
                << std::endl;
        }

        // RGA 同步转换完成，归还摄像头 MMAP 缓冲区。
        camera.releaseFrame();

        // 3. 提交未画框的 NV12，交给独立检测线程。
        // submit() 内部复制数据，不保留 converter 的缓冲区指针。
        const auto submit_t0 = Clock::now();

        const bool submitted = asyncDetector.submit(nv12);

        // 获取最近一次已完成的检测结果。
        // 此处不等待当前帧推理完成。
        asyncDetector.latest(detections);

        const auto submit_t1 = Clock::now();

        if (!submitted && debugCount % 30 == 0)
        {
            std::cerr << "[RKNN] async submit failed" << std::endl;
        }

        // 4. 在当前 NV12 视频帧上画最近一次检测框。
        const auto draw_t0 = Clock::now();

        drawDetectionsNV12(nv12, detections, 2);

        const auto draw_t1 = Clock::now();

        if (debugCount % 30 == 0)
        {
            const double submit_ms =
                std::chrono::duration<double, std::milli>(
                    submit_t1 - submit_t0
                ).count();

            const double draw_ms =
                std::chrono::duration<double, std::milli>(
                    draw_t1 - draw_t0
                ).count();

            std::cout
                << "[TIME] async_submit+latest="
                << submit_ms
                << " ms draw="
                << draw_ms
                << " ms persons="
                << detections.size()
                << std::endl;
        }

        // 5. 将画好框的 NV12 送入 MPP 编码。
        int h264Size = 0;

        const auto encode_t0 = Clock::now();

        const bool encoded = encoder.encode(
            nv12,
            h264Buffer.data(),
            h264Buffer.size(),
            h264Size
        );

        const auto encode_t1 = Clock::now();

        if (debugCount % 30 == 0)
        {
            const double encode_ms =
                std::chrono::duration<double, std::milli>(
                    encode_t1 - encode_t0
                ).count();

            std::cout
                << "[TIME] MPP encode="
                << encode_ms
                << " ms size="
                << h264Size
                << std::endl;
        }

        // 清空帧信息，转换器内部的 vector 缓冲区继续复用。
        converter.release(nv12);

        // 6. 推送编码结果。
        if (encoded && h264Size > 0)
        {
            ++encodedFrames;

            const auto rtsp_t0 = Clock::now();

            const bool pushed = rtsp.pushFrame(
                h264Buffer.data(),
                h264Size,
                ptsUs
            );

            const auto rtsp_t1 = Clock::now();

            if (debugCount % 30 == 0)
            {
                const double rtsp_ms =
                    std::chrono::duration<double, std::milli>(
                        rtsp_t1 - rtsp_t0
                    ).count();

                std::cout
                    << "[TIME] RTSP push="
                    << rtsp_ms
                    << " ms"
                    << std::endl;
            }

            if (pushed)
            {
                ++rtspFrames;
            }
        }

        // 每秒打印主线程的采集、编码和推流帧率。
        const Clock::time_point now = Clock::now();

        const double seconds =
            std::chrono::duration<double>(
                now - lastPrint
            ).count();

        if (seconds >= 1.0)
        {
            std::cout
                << std::fixed
                << std::setprecision(2)
                << "[PERF] Capture="
                << totalFrames / seconds
                << " FPS | Encode="
                << encodedFrames / seconds
                << " FPS | RTSP="
                << rtspFrames / seconds
                << " FPS"
                << std::endl;

            totalFrames = 0;
            encodedFrames = 0;
            rtspFrames = 0;

            lastPrint = now;
        }
    }

    std::cout << "Stopping..." << std::endl;

    // 先等待检测线程结束，确保模型不再被使用。
    asyncDetector.stop();

    rtsp.stop();
    encoder.release();
    camera.stop();

    std::cout << "Exit." << std::endl;

    return 0;
}