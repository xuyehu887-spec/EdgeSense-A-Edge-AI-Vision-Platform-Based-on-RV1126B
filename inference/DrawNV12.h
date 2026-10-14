#pragma once
#include <vector>
#include "VideoFrame.h"
#include "YoloV8Detector.h"
void drawDetectionsNV12(VideoFrame& frame,
                        const std::vector<Detection>& detections,
                        int thickness = 2);
