#include "RtspServer.h"

#include <iostream>
#include <cstring>


RtspServer::RtspServer()
:
server_(nullptr),
factory_(nullptr),
appsrc_(nullptr),
loop_(nullptr),
running_(false),
port_(8554),
gst_initialized_(false)
{

}


RtspServer::~RtspServer()
{
    stop();
}



bool RtspServer::initGStreamer()
{
    if(gst_initialized_)
        return true;


    gst_init(
        nullptr,
        nullptr
    );


    gst_initialized_=true;


    std::cout
    << "[RTSP] GStreamer init OK"
    << std::endl;


    return true;
}



bool RtspServer::start(int port)
{

    if(running_)
        return true;


    initGStreamer();


    port_=port;


    server_ =
        gst_rtsp_server_new();


    if(server_==nullptr)
        return false;



    char service[32];

    snprintf(
        service,
        sizeof(service),
        "%d",
        port_
    );


    gst_rtsp_server_set_service(
        server_,
        service
    );



    factory_ =
        gst_rtsp_media_factory_new();



    /*
        低延迟 pipeline

    */

    const char* pipeline =

    "( "
    
    "appsrc name=src "
    "is-live=true "
    "block=false "
    "format=time "

    "caps=\"video/x-h264,"
    "stream-format=(string)byte-stream,"
    "alignment=(string)au\" "

    "! h264parse "

    "config-interval=1 "

    "! rtph264pay "

    "name=pay0 "
    "pt=96 "

    "config-interval=1 "

    ")";



    gst_rtsp_media_factory_set_launch(
        factory_,
        pipeline
    );



    gst_rtsp_media_factory_set_shared(
        factory_,
        TRUE
    );



    g_signal_connect(
        factory_,
        "media-configure",
        G_CALLBACK(
            RtspServer::mediaConfigure
        ),
        this
    );



    /*
       删除 media-unprepared
       你的GStreamer版本没有这个signal
    */



    GstRTSPMountPoints* mounts =
        gst_rtsp_server_get_mount_points(
            server_
        );


    gst_rtsp_mount_points_add_factory(
        mounts,
        "/live",
        factory_
    );


    gst_object_unref(
        mounts
    );



    loop_ =
        g_main_loop_new(
            nullptr,
            FALSE
        );



    gst_rtsp_server_attach(
        server_,
        nullptr
    );


    running_=true;



    thread_ =
        std::thread(
        [this]()
        {

            std::cout
            << "[RTSP] loop start"
            << std::endl;


            g_main_loop_run(
                loop_
            );


            std::cout
            << "[RTSP] loop exit"
            << std::endl;


        });



    std::cout
    << "[RTSP] started:"
    << port_
    << std::endl;



    return true;

}




void RtspServer::mediaConfigure(
    GstRTSPMediaFactory* factory,
    GstRTSPMedia* media,
    gpointer user_data
)
{

    RtspServer* self =
        static_cast<RtspServer*>(
            user_data
        );


    if(self==nullptr)
        return;



    GstElement* element =
        gst_rtsp_media_get_element(
            media
        );


    if(element==nullptr)
        return;



    GstElement* src =
        gst_bin_get_by_name(
            GST_BIN(element),
            "src"
        );



    if(src)
    {


        g_object_set(
            G_OBJECT(src),

            "is-live",
            TRUE,

            "block",
            FALSE,

            "format",
            GST_FORMAT_TIME,

            "do-timestamp",
            FALSE,

            nullptr
        );



        self->setAppSrc(
            src
        );

        // 获取rtph264pay
        GstElement* pay =
            gst_bin_get_by_name(
                GST_BIN(element),
                "pay0"
            );


        if(pay)
        {
            GstPad* srcpad =
                gst_element_get_static_pad(
                    pay,
                    "src"
                );


            if(srcpad)
            {

                gst_pad_add_probe(
                    srcpad,
                    GST_PAD_PROBE_TYPE_BUFFER,
                    RtspServer::payProbe,
                    self,
                    nullptr
                );


                gst_object_unref(srcpad);
            }


            gst_object_unref(pay);
        }

        gst_object_unref(
            src
        );

    }



    gst_object_unref(
        element
    );

}






void RtspServer::setAppSrc(
    GstElement* src
)
{

    std::lock_guard<std::mutex>
    lock(
        mutex_
    );



    if(appsrc_)
    {
        gst_object_unref(
            appsrc_
        );
    }



    appsrc_ =
        GST_ELEMENT(
            gst_object_ref(
                src
            )
        );

}







void RtspServer::clearAppSrc()
{

    std::lock_guard<std::mutex>
    lock(
        mutex_
    );



    if(appsrc_)
    {

        gst_object_unref(
            appsrc_
        );


        appsrc_=nullptr;

    }

}







bool RtspServer::pushFrame(
    const uint8_t* data,
    int size,
    uint64_t timestamp
)
{


    if(!running_)
        return false;



    if(data==nullptr || size<=0)
        return false;



    GstElement* src=nullptr;



    {

        std::lock_guard<std::mutex>
        lock(
            mutex_
        );



        if(appsrc_==nullptr)
            return false;



        src =
        GST_ELEMENT(
            gst_object_ref(
                appsrc_
            )
        );

    }



    GstBuffer* buffer =
        gst_buffer_new_allocate(
            nullptr,
            size,
            nullptr
        );



    if(buffer==nullptr)
    {
        gst_object_unref(src);
        return false;
    }




    GstMapInfo map;


    gst_buffer_map(
        buffer,
        &map,
        GST_MAP_WRITE
    );



    memcpy(
        map.data,
        data,
        size
    );



    gst_buffer_unmap(
        buffer,
        &map
    );



    /*
       V4L2 timestamp:
       us

       GStreamer:
       ns


       以第一帧为0点
    */


    static uint64_t first_timestamp = 0;


    if(first_timestamp==0)
    {
        first_timestamp = timestamp;
    }


    uint64_t pts =
        (timestamp-first_timestamp)
        *
        1000;



    GST_BUFFER_PTS(buffer)
        =
        pts;


    GST_BUFFER_DTS(buffer)
        =
        pts;



    /*
       根据真实20FPS左右
       不再强制30FPS

    */

    GST_BUFFER_DURATION(buffer)
        =
        GST_SECOND / 20;



    GST_BUFFER_OFFSET(buffer)
        =
        GST_BUFFER_OFFSET_NONE;



    GstFlowReturn ret;

    appsrc_time_ =std::chrono::steady_clock::now();
    g_signal_emit_by_name(
        src,
        "push-buffer",
        buffer,
        &ret
    );



    gst_buffer_unref(
        buffer
    );


    gst_object_unref(
        src
    );




    if(ret != GST_FLOW_OK)
    {

        std::cerr
        << "[RTSP] push failed "
        << ret
        << std::endl;


        return false;

    }



    return true;

}








void RtspServer::stop()
{

    if(!running_)
        return;



    running_=false;



    clearAppSrc();



    if(loop_)
    {

        g_main_loop_quit(
            loop_
        );

    }



    if(thread_.joinable())
    {
        thread_.join();
    }




    if(loop_)
    {

        g_main_loop_unref(
            loop_
        );


        loop_=nullptr;

    }



    if(server_)
    {

        g_object_unref(
            server_
        );


        server_=nullptr;

    }



    factory_=nullptr;



    std::cout
    << "[RTSP] stopped"
    << std::endl;

}





bool RtspServer::isRunning() const
{
    return running_;
}
GstPadProbeReturn RtspServer::payProbe(
    GstPad* pad,
    GstPadProbeInfo* info,
    gpointer user_data
)
{

    RtspServer* self =
        static_cast<RtspServer*>(user_data);


    auto now =
        std::chrono::steady_clock::now();



    double ms =
        std::chrono::duration<double,std::milli>(
            now-self->appsrc_time_
        ).count();



    std::cout
        << "[GST] appsrc->rtph264pay="
        << ms
        << " ms"
        << std::endl;



    return GST_PAD_PROBE_OK;
}