#ifndef SMARTVISION_CAMERA_MANAGER_H
#define SMARTVISION_CAMERA_MANAGER_H


#include <linux/videodev2.h>

#include <stdint.h>
#include <stddef.h>


#include "../video/VideoFrame.h"



#define BUFFER_COUNT 4



class CameraManager
{

public:


    CameraManager();


    ~CameraManager();



    bool open(
        const char* device,
        int width,
        int height,
        int fps
    );



    bool start();



    bool getFrame(
        VideoFrame& frame
    );



    void releaseFrame();



    void stop();



    bool isOpened() const;



private:


    bool setFormat();


    bool requestBuffers();


    bool mmapBuffers();


    bool queueBuffers();



private:


    int fd_;


    int width_;

    int height_;

    int fps_;
    int stride_bytes_;

    int current_buffer_index_;
    bool streaming_;



    struct Buffer
    {

        void* start;

        size_t length;

    };



    Buffer buffers_[BUFFER_COUNT];



    int currentIndex_;


};


#endif