#pragma once
#include <cstdint>
#include <vector>
#include "VideoFrame.h"
#include "rknn_api.h"

struct Detection {
    float x1, y1, x2, y2, score;
    int class_id;
};

// 同步接口，非线程安全。支持紧密排列的 NV12。
// 针对所上传模型：std=[1,1,1]，输入范围0..1。
// 输入：RGB float32，C++ 侧除以255；不要用于已内置 /255 的其他模型。
// 输出格式：已解码 [1,4+C,N]/[1,N,4+C]，或三尺度
// [DFL box, sigmoid class] x3 / [DFL box, sigmoid class, score_sum] x3。
class YoloV8Detector {
public:
    YoloV8Detector();
    ~YoloV8Detector();
    YoloV8Detector(const YoloV8Detector&) = delete;
    YoloV8Detector& operator=(const YoloV8Detector&) = delete;
    bool init(const char* modelPath);
    bool detect(const VideoFrame& nv12, std::vector<Detection>& detections);
    void release();
    void setThresholds(float confidence, float nms);
    void setPersonClassId(int classId);
private:
    rknn_context ctx_ = 0;
    int width_ = 0, height_ = 0;
    float confidence_ = 0.25f, nms_ = 0.45f;
    int person_ = 0;
    std::vector<rknn_tensor_attr> attrs_;
    std::vector<uint8_t> rgb_, input_;
    std::vector<float> normalized_input_;
};
