#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H


#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>



class RtspServer
{

public:

    RtspServer();

    ~RtspServer();



    bool start(
        int port = 8554
    );


    void stop();



    bool pushFrame(
        const uint8_t* data,
        int size,
        uint64_t timestamp
    );



    bool isRunning() const;



private:


    bool initGStreamer();



    static void mediaConfigure(
        GstRTSPMediaFactory* factory,
        GstRTSPMedia* media,
        gpointer user_data
    );



    static void mediaUnprepared(
        GstRTSPMediaFactory* factory,
        GstRTSPMedia* media,
        gpointer user_data
    );



    void setAppSrc(
        GstElement* src
    );


    void clearAppSrc();



private:
    static GstPadProbeReturn payProbe(
        GstPad* pad,
        GstPadProbeInfo* info,
        gpointer user_data
    );


    std::chrono::steady_clock::time_point appsrc_time_;

    GstRTSPServer* server_;


    GstRTSPMediaFactory* factory_;


    GstElement* appsrc_;


    GMainLoop* loop_;


    std::thread thread_;


    std::mutex mutex_;


    std::atomic<bool> running_;


    int port_;


    bool gst_initialized_;



};


#endif