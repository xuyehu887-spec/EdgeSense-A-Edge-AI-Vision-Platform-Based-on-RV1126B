#include "CameraManager.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <chrono>
#include <cstring>

using namespace std;

#define CLEAR(x) memset(&(x),0,sizeof(x))

static int xioctl(
        int fd,
        int request,
        void* arg)
{

    int r;


    do
    {

        r = ioctl(
                fd,
                request,
                arg
        );


    }while(
        r < 0 &&
        errno == EINTR
    );


    return r;

}


CameraManager::CameraManager()
{
    fd_ = -1;

    width_ = 0;
    height_ = 0;
    fps_ = 0;

    // 新增：初始化每行字节数。
    stride_bytes_ = 0;

    streaming_ = false;
    currentIndex_ = -1;

    for (int i = 0; i < BUFFER_COUNT; ++i)
    {
        buffers_[i].start = nullptr;
        buffers_[i].length = 0;
    }
}



CameraManager::~CameraManager()
{

    stop();

}




bool CameraManager::open(
        const char* device,
        int width,
        int height,
        int fps)
{


    width_=width;

    height_=height;

    fps_=fps;



    fd_=::open(
            device,
            O_RDWR
    );


    if(fd_<0)
    {

        perror("open camera");

        return false;

    }



    cout<<"[Camera] open "
        <<device
        <<endl;



    return setFormat();

}




bool CameraManager::setFormat()
{
    struct v4l2_format fmt;
    CLEAR(fmt);

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width_;
    fmt.fmt.pix.height = height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
    {
        perror("VIDIOC_S_FMT");
        return false;
    }

    // 驱动可能调整请求的格式或尺寸。
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
    {
        std::cerr
            << "[Camera] returned format is not YUYV"
            << std::endl;
        return false;
    }

    // 当前 main.cpp 按固定尺寸初始化 MPP。
    // 若摄像头调整尺寸，先明确报错，避免编码尺寸不匹配。
    if (fmt.fmt.pix.width != static_cast<unsigned int>(width_) ||
        fmt.fmt.pix.height != static_cast<unsigned int>(height_))
    {
        std::cerr
            << "[Camera] unexpected size: "
            << fmt.fmt.pix.width << "x"
            << fmt.fmt.pix.height
            << ", requested "
            << width_ << "x" << height_
            << std::endl;
        return false;
    }

    stride_bytes_ =
        static_cast<int>(fmt.fmt.pix.bytesperline);

    if (stride_bytes_ == 0)
    {
        stride_bytes_ = width_ * 2;
    }

    if (stride_bytes_ < width_ * 2 ||
        (stride_bytes_ % 2) != 0)
    {
        std::cerr
            << "[Camera] invalid YUYV stride: "
            << stride_bytes_
            << std::endl;
        return false;
    }

    std::cout
        << "[Camera] format "
        << width_ << "x" << height_
        << " YUYV, stride="
        << stride_bytes_
        << " bytes"
        << std::endl;

    return true;
}




bool CameraManager::requestBuffers()
{


    struct v4l2_requestbuffers req;


    CLEAR(req);



    req.count =
        BUFFER_COUNT;



    req.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;



    req.memory =
        V4L2_MEMORY_MMAP;



    if(
        xioctl(
            fd_,
            VIDIOC_REQBUFS,
            &req
        ) <0
    )
    {

        perror("VIDIOC_REQBUFS");

        return false;

    }



    if(req.count < BUFFER_COUNT)
    {

        cerr<<"not enough buffer"<<endl;

        return false;

    }



    return true;

}





bool CameraManager::mmapBuffers()
{


    for(int i=0;i<BUFFER_COUNT;i++)
    {


        struct v4l2_buffer buf;


        CLEAR(buf);



        buf.type =
            V4L2_BUF_TYPE_VIDEO_CAPTURE;


        buf.memory =
            V4L2_MEMORY_MMAP;


        buf.index=i;



        if(
            xioctl(
                fd_,
                VIDIOC_QUERYBUF,
                &buf
            )<0
        )
        {

            perror("QUERYBUF");

            return false;

        }



        buffers_[i].length =
                buf.length;



        buffers_[i].start =
            mmap(
                NULL,
                buf.length,
                PROT_READ|PROT_WRITE,
                MAP_SHARED,
                fd_,
                buf.m.offset
            );



        if(
            buffers_[i].start
            ==
            MAP_FAILED
        )
        {

            perror("mmap");

            return false;

        }


    }


    return true;

}






bool CameraManager::queueBuffers()
{


    for(int i=0;i<BUFFER_COUNT;i++)
    {

        struct v4l2_buffer buf;


        CLEAR(buf);



        buf.type =
            V4L2_BUF_TYPE_VIDEO_CAPTURE;



        buf.memory =
            V4L2_MEMORY_MMAP;



        buf.index=i;



        if(
            xioctl(
                fd_,
                VIDIOC_QBUF,
                &buf
            )<0
        )
        {

            perror("QBUF");

            return false;

        }

    }



    return true;

}






bool CameraManager::start()
{


    if(
        requestBuffers()==false
    )
        return false;



    if(
        mmapBuffers()==false
    )
        return false;



    if(
        queueBuffers()==false
    )
        return false;



    enum v4l2_buf_type type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;



    if(
        xioctl(
            fd_,
            VIDIOC_STREAMON,
            &type
        )<0
    )
    {

        perror("STREAMON");

        return false;

    }



    streaming_=true;



    cout<<"[Camera] streaming start"
        <<endl;



    return true;

}







bool CameraManager::getFrame(
        VideoFrame& frame)
{


    struct v4l2_buffer buf;

    CLEAR(buf);



    buf.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;


    buf.memory =
        V4L2_MEMORY_MMAP;



    if(
        xioctl(
            fd_,
            VIDIOC_DQBUF,
         &buf
        )<0
    )
    {
        perror("DQBUF");
        return false;
    }

    currentIndex_=buf.index;



    frame.data =
        (uint8_t*)
        buffers_[buf.index].start;
    frame.size =
        buf.bytesused;



    frame.width =
        width_;


    frame.height =
        height_;



    frame.format =
        PIXEL_FORMAT_YUYV;
    frame.stride_bytes = stride_bytes_;


    frame.timestamp =
        buf.timestamp.tv_sec*1000000
        +
        buf.timestamp.tv_usec;

    return true;

}




void CameraManager::releaseFrame()
{


    if(currentIndex_<0)
        return;



    struct v4l2_buffer buf;


    CLEAR(buf);



    buf.type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;



    buf.memory =
        V4L2_MEMORY_MMAP;



    buf.index =
        currentIndex_;



    xioctl(
        fd_,
        VIDIOC_QBUF,
        &buf
    );



    currentIndex_=-1;


}






void CameraManager::stop()
{


    if(fd_<0)
        return;



    if(streaming_)
    {


        enum v4l2_buf_type type =
            V4L2_BUF_TYPE_VIDEO_CAPTURE;



        xioctl(
            fd_,
            VIDIOC_STREAMOFF,
            &type
        );


        streaming_=false;

    }




    for(int i=0;i<BUFFER_COUNT;i++)
    {

        if(buffers_[i].start)
        {

            munmap(
                buffers_[i].start,
                buffers_[i].length
            );

        }

    }




    close(fd_);


    fd_=-1;



}





bool CameraManager::isOpened() const
{

    return fd_>=0;

}