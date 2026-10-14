#pragma once
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include "YoloV8Detector.h"

// detector 必须先 init，并且生命周期长于此对象。
// start 后只由 worker 调用 detector.detect；主线程不可再调用 detect/release。
// 输入为本项目 RGA 输出的紧密排列 NV12，内部复制，不保留转换器指针。
class AsyncPersonDetector {
public:
    explicit AsyncPersonDetector(YoloV8Detector& detector) : detector_(detector) {}
    ~AsyncPersonDetector() { stop(); }
    AsyncPersonDetector(const AsyncPersonDetector&) = delete;
    AsyncPersonDetector& operator=(const AsyncPersonDetector&) = delete;

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable()) return;
        stopping_ = false;
        pending_ = false;
        result_.clear();
        thread_ = std::thread(&AsyncPersonDetector::run, this);
    }

    bool submit(const VideoFrame& frame) {
        if (!frame.data || frame.width <= 0 || frame.height <= 0 ||
            (frame.width & 1) || (frame.height & 1) ||
            frame.format != PIXEL_FORMAT_NV12 || frame.size <= 0) return false;
        const uint64_t bytes = static_cast<uint64_t>(frame.width) * frame.height * 3 / 2;
        if (bytes > static_cast<uint64_t>(frame.size)) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || !thread_.joinable()) return false;
            pending_data_.resize(static_cast<size_t>(bytes));
            std::memcpy(pending_data_.data(), frame.data, static_cast<size_t>(bytes));
            pending_frame_ = frame;
            pending_frame_.data = nullptr;
            pending_frame_.size = static_cast<int>(bytes);
            pending_ = true; // 覆盖尚未处理的旧帧，不建立排队队列。
        }
        cv_.notify_one();
        return true;
    }

    void latest(std::vector<Detection>& boxes) {
        std::lock_guard<std::mutex> lock(mutex_);
        boxes = result_;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        std::vector<uint8_t> working;
        while (true) {
            VideoFrame frame{};
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || pending_; });
                if (stopping_) break;
                working.swap(pending_data_);
                frame = pending_frame_;
                frame.data = working.data();
                pending_ = false;
            }
            // 推理在锁外执行，不阻塞主线程提交或读取结果。
            std::vector<Detection> boxes;
            bool ok = false;
            try {
                ok = detector_.detect(frame, boxes);
            } catch (const std::exception& e) {
                std::cerr << "[AsyncDetect] " << e.what() << std::endl;
            }
            if (!ok) boxes.clear();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                result_.swap(boxes);
            }
        }
    }
    YoloV8Detector& detector_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false, pending_ = false;
    VideoFrame pending_frame_{};
    std::vector<uint8_t> pending_data_;
    std::vector<Detection> result_;
};
