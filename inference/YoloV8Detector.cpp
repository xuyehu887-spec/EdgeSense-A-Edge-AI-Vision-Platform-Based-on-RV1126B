#include "YoloV8Detector.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
float clip(float v, float lo, float hi) { return std::max(lo, std::min(v, hi)); }
uint8_t byte(int v) { return static_cast<uint8_t>(std::max(0, std::min(v,255))); }
void printAttr(const char* tag, const rknn_tensor_attr& a) {
    std::cout << "[RKNN] " << tag << " index=" << a.index << " name=" << a.name << " dims=[";
    for (uint32_t i=0; i<a.n_dims; ++i) std::cout << (i ? "," : "") << a.dims[i];
    std::cout << "] fmt=" << int(a.fmt) << " type=" << int(a.type)
              << " qnt=" << int(a.qnt_type) << " zp=" << a.zp << " scale=" << a.scale << std::endl;
}
// 本项目 RGA 输出 stride_bytes=width，Y 平面后接 UV 平面。
// BT.601 limited-range；若摄像头使用其他色彩范围，需要相应调整。
bool toRGB(const VideoFrame& f, std::vector<uint8_t>& rgb) {
    if (!f.data || f.format != PIXEL_FORMAT_NV12 || f.width<=0 || f.height<=0 ||
        (f.width&1) || (f.height&1) || f.size<=0) return false;
    const uint64_t n = static_cast<uint64_t>(f.width)*f.height;
    if (n*3/2 > static_cast<uint64_t>(f.size)) return false;
    rgb.resize(static_cast<size_t>(n)*3);
    const uint8_t* uv = f.data + static_cast<size_t>(n);
    for (int y=0; y<f.height; ++y) for (int x=0; x<f.width; ++x) {
        const size_t i=static_cast<size_t>(y)*f.width+x;
        const size_t j=static_cast<size_t>(y/2)*f.width+(x&~1);
        const int c=std::max(0,int(f.data[i])-16), d=int(uv[j])-128, e=int(uv[j+1])-128;
        rgb[i*3]=byte((298*c+409*e+128)>>8);
        rgb[i*3+1]=byte((298*c-100*d-208*e+128)>>8);
        rgb[i*3+2]=byte((298*c+516*d+128)>>8);
    }
    return true;
}
struct Letterbox { int left, top; float sx, sy; };
Letterbox letterbox(const std::vector<uint8_t>& src, int sw, int sh,
                    std::vector<uint8_t>& dst, int dw, int dh) {
    const float s=std::min(float(dw)/sw,float(dh)/sh);
    const int rw=std::max(1,std::min(dw,int(std::round(sw*s))));
    const int rh=std::max(1,std::min(dh,int(std::round(sh*s))));
    Letterbox lb{(dw-rw)/2,(dh-rh)/2,float(rw)/sw,float(rh)/sh};
    dst.assign(static_cast<size_t>(dw)*dh*3,114);
    for (int y=0; y<rh; ++y) {
        const float sy=clip((y+0.5f)/lb.sy-0.5f,0,float(sh-1));
        const int y0=int(sy), y1=std::min(y0+1,sh-1); const float fy=sy-y0;
        for (int x=0; x<rw; ++x) {
            const float sx=clip((x+0.5f)/lb.sx-0.5f,0,float(sw-1));
            const int x0=int(sx), x1=std::min(x0+1,sw-1); const float fx=sx-x0;
            for (int c=0; c<3; ++c) {
                const float a=src[(static_cast<size_t>(y0)*sw+x0)*3+c];
                const float b=src[(static_cast<size_t>(y0)*sw+x1)*3+c];
                const float d=src[(static_cast<size_t>(y1)*sw+x0)*3+c];
                const float e=src[(static_cast<size_t>(y1)*sw+x1)*3+c];
                const float v=(1-fy)*((1-fx)*a+fx*b)+fy*((1-fx)*d+fx*e);
                dst[(static_cast<size_t>(y+lb.top)*dw+x+lb.left)*3+c]=byte(int(std::round(v)));
            }
        }
    }
    return lb;
}
float iou(const Detection& a, const Detection& b) {
    const float w=std::max(0.0f,std::min(a.x2,b.x2)-std::max(a.x1,b.x1));
    const float h=std::max(0.0f,std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
    const float inter=w*h;
    const float uni=(a.x2-a.x1)*(a.y2-a.y1)+(b.x2-b.x1)*(b.y2-b.y1)-inter;
    return uni>0 ? inter/uni : 0;
}
void nms(std::vector<Detection>& boxes,float threshold,std::vector<Detection>& out) {
    std::sort(boxes.begin(),boxes.end(),[](const Detection& a,const Detection& b){return a.score>b.score;});
    // 限制最坏情况下的 NMS 工作量。
    if (boxes.size()>3000) boxes.resize(3000);
    std::vector<bool> drop(boxes.size(),false);
    for (size_t i=0; i<boxes.size(); ++i) {
        if (drop[i]) continue;
        out.push_back(boxes[i]);
        if (out.size()>=300) break;
        for (size_t j=i+1; j<boxes.size(); ++j)
            if (!drop[j] && boxes[i].class_id==boxes[j].class_id && iou(boxes[i],boxes[j])>threshold) drop[j]=true;
    }
}
struct Map {
    const float* data; int c,h,w; bool nhwc;
    float at(int ch,int y,int x) const {
        return nhwc ? data[(static_cast<size_t>(y)*w+x)*c+ch]
                    : data[(static_cast<size_t>(ch)*h+y)*w+x];
    }
};
bool getMap(const rknn_tensor_attr& a,const rknn_output& o,Map& m) {
    if (a.n_dims!=4 || a.dims[0]!=1 || !o.buf ||
        (a.fmt!=RKNN_TENSOR_NCHW && a.fmt!=RKNN_TENSOR_NHWC)) return false;
    m.data=static_cast<const float*>(o.buf); m.nhwc=a.fmt==RKNN_TENSOR_NHWC;
    m.c=int(a.dims[m.nhwc?3:1]); m.h=int(a.dims[m.nhwc?1:2]); m.w=int(a.dims[m.nhwc?2:3]);
    return m.c>0 && m.h>0 && m.w>0 &&
        static_cast<uint64_t>(m.c)*m.h*m.w*sizeof(float)<=o.size;
}
float dfl(const Map& m,int side,int bins,int y,int x) {
    float maximum=-std::numeric_limits<float>::infinity();
    for (int k=0; k<bins; ++k) maximum=std::max(maximum,m.at(side*bins+k,y,x));
    float sum=0, weighted=0;
    for (int k=0; k<bins; ++k) {
        const float p=std::exp(m.at(side*bins+k,y,x)-maximum); sum+=p; weighted+=p*k;
    }
    return weighted/sum;
}
struct OutputGuard {
    rknn_context ctx; std::vector<rknn_output>& outputs;
    ~OutputGuard() { rknn_outputs_release(ctx,uint32_t(outputs.size()),outputs.data()); }
};
}

YoloV8Detector::YoloV8Detector() {}
YoloV8Detector::~YoloV8Detector() { release(); }
void YoloV8Detector::release() {
    if (ctx_) { rknn_destroy(ctx_); ctx_=0; }
    attrs_.clear(); width_=height_=0;
}
void YoloV8Detector::setThresholds(float confidence,float threshold) {
    if (std::isfinite(confidence)) confidence_=clip(confidence,0,1);
    if (std::isfinite(threshold)) nms_=clip(threshold,0,1);
}
void YoloV8Detector::setPersonClassId(int id) { if (id>=0) person_=id; }

bool YoloV8Detector::init(const char* path) {
    release();
    if (!path) return false;
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if (!f) { std::cerr<<"[RKNN] cannot open model: "<<path<<std::endl; return false; }
    const std::streamoff size=f.tellg();
    if (size<=0 || uint64_t(size)>std::numeric_limits<uint32_t>::max()) return false;
    std::vector<uint8_t> model(static_cast<size_t>(size)); f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(model.data()),std::streamsize(size))) return false;
    int ret=rknn_init(&ctx_,model.data(),uint32_t(model.size()),0,nullptr);
    if (ret!=RKNN_SUCC) { std::cerr<<"[RKNN] init failed ret="<<ret<<std::endl; release(); return false; }
    rknn_input_output_num io{};
    ret=rknn_query(ctx_,RKNN_QUERY_IN_OUT_NUM,&io,sizeof(io));
    if (ret!=RKNN_SUCC || io.n_input!=1 || !io.n_output) {
        std::cerr<<"[RKNN] requires a single input"<<std::endl; release(); return false;
    }
    rknn_tensor_attr a{}; a.index=0;
    ret=rknn_query(ctx_,RKNN_QUERY_INPUT_ATTR,&a,sizeof(a));
    if (ret!=RKNN_SUCC) { release(); return false; }
    printAttr("input",a);
    int channels=0;
    if (a.n_dims==4 && a.dims[0]==1) {
        if (a.fmt==RKNN_TENSOR_NHWC) { height_=int(a.dims[1]); width_=int(a.dims[2]); channels=int(a.dims[3]); }
        else if (a.fmt==RKNN_TENSOR_NCHW) { channels=int(a.dims[1]); height_=int(a.dims[2]); width_=int(a.dims[3]); }
    }
    if (channels!=3 || width_<=0 || height_<=0 ||
        uint64_t(width_)*height_*3*sizeof(float)>std::numeric_limits<uint32_t>::max()) {
        std::cerr<<"[RKNN] unsupported RGB input"<<std::endl; release(); return false;
    }
    attrs_.resize(io.n_output);
    for (uint32_t i=0; i<io.n_output; ++i) {
        std::memset(&attrs_[i],0,sizeof(rknn_tensor_attr)); attrs_[i].index=i;
        ret=rknn_query(ctx_,RKNN_QUERY_OUTPUT_ATTR,&attrs_[i],sizeof(rknn_tensor_attr));
        if (ret!=RKNN_SUCC) { release(); return false; }
        printAttr("output",attrs_[i]);
    }
    std::cout<<"[RKNN] input="<<width_<<"x"<<height_<<", outputs="<<io.n_output<<std::endl;
    return true;
}

bool YoloV8Detector::detect(const VideoFrame& frame,std::vector<Detection>& out) {
    out.clear();
    if (!ctx_ || !toRGB(frame,rgb_)) { std::cerr<<"[RKNN] invalid context/NV12"<<std::endl; return false; }
    const Letterbox lb=letterbox(rgb_,frame.width,frame.height,input_,width_,height_);
    // 此 RKNN 的配置为 mean=0,std=1；原始网络输入范围是0..1。
    // Runtime 负责 FLOAT32 到模型内部类型的转换，不负责本模型缺失的 /255。
    normalized_input_.resize(input_.size());
    for (size_t i=0; i<input_.size(); ++i)
        normalized_input_[i]=static_cast<float>(input_[i]) / 255.0f;
    rknn_input in{}; in.index=0; in.buf=normalized_input_.data();
    in.size=uint32_t(normalized_input_.size()*sizeof(float));
    in.type=RKNN_TENSOR_FLOAT32; in.fmt=RKNN_TENSOR_NHWC; in.pass_through=0;
    int ret=rknn_inputs_set(ctx_,1,&in);
    if (ret!=RKNN_SUCC) { std::cerr<<"[RKNN] inputs_set ret="<<ret<<std::endl; return false; }
    ret=rknn_run(ctx_,nullptr);
    if (ret!=RKNN_SUCC) { std::cerr<<"[RKNN] run ret="<<ret<<std::endl; return false; }
    std::vector<rknn_output> outputs(attrs_.size());
    for (size_t i=0; i<outputs.size(); ++i) {
        std::memset(&outputs[i],0,sizeof(rknn_output)); outputs[i].index=uint32_t(i);
        outputs[i].want_float=1; outputs[i].is_prealloc=0;
    }
    ret=rknn_outputs_get(ctx_,uint32_t(outputs.size()),outputs.data(),nullptr);
    if (ret!=RKNN_SUCC) { std::cerr<<"[RKNN] outputs_get ret="<<ret<<std::endl; return false; }
    OutputGuard guard{ctx_,outputs};
    std::vector<Detection> boxes;
    const auto add=[&](float x1,float y1,float x2,float y2,float score) {
        if (!std::isfinite(x1)||!std::isfinite(y1)||!std::isfinite(x2)||!std::isfinite(y2)||
            !std::isfinite(score)||score<confidence_||score>1) return;
        Detection d{clip((x1-lb.left)/lb.sx,0,float(frame.width)),
                    clip((y1-lb.top)/lb.sy,0,float(frame.height)),
                    clip((x2-lb.left)/lb.sx,0,float(frame.width)),
                    clip((y2-lb.top)/lb.sy,0,float(frame.height)),score,person_};
        if (d.x2>d.x1 && d.y2>d.y1) boxes.push_back(d);
    };
    if (outputs.size()==1) {
        const rknn_tensor_attr& a=attrs_[0];
        if (a.n_dims!=3 || a.dims[0]!=1) {
            std::cerr<<"[RKNN] unsupported single output; expected [1,4+C,N] or [1,N,4+C]"<<std::endl; return false;
        }
        const int d1=int(a.dims[1]),d2=int(a.dims[2]);
        const bool cf=d1>=5 && d1<=84 && d2>d1;
        const bool cl=d2>=5 && d2<=84 && d1>d2;
        if (!cf && !cl) { std::cerr<<"[RKNN] unsupported decoded layout"<<std::endl; return false; }
        const int c=cf?d1:d2,n=cf?d2:d1;
        if (person_>=c-4 || !outputs[0].buf || uint64_t(c)*n*sizeof(float)>outputs[0].size) return false;
        const float* p=static_cast<const float*>(outputs[0].buf);
        const auto value=[&](int ch,int i) {return cf?p[static_cast<size_t>(ch)*n+i]:p[static_cast<size_t>(i)*c+ch];};
        for (int i=0; i<n; ++i) {
            const float score=value(4+person_,i); if (score<confidence_) continue;
            const float x=value(0,i),y=value(1,i),w=value(2,i),h=value(3,i);
            add(x-w/2,y-h/2,x+w/2,y+h/2,score);
        }
    } else if (outputs.size()==6 || outputs.size()==9) {
        const size_t group=outputs.size()/3;
        for (size_t branch=0; branch<3; ++branch) {
            const size_t bi=branch*group,ci=bi+1; Map b{},c{};
            if (!getMap(attrs_[bi],outputs[bi],b)||!getMap(attrs_[ci],outputs[ci],c)||
                b.c%4 || b.c/4<2 || b.c/4>64 || b.w!=c.w || b.h!=c.h || person_>=c.c) {
                std::cerr<<"[RKNN] unsupported DFL/class outputs or order"<<std::endl; return false;
            }
            const int bins=b.c/4; const float sx=float(width_)/b.w,sy=float(height_)/b.h;
            for (int y=0; y<b.h; ++y) for (int x=0; x<b.w; ++x) {
                const float score=c.at(person_,y,x); if (!std::isfinite(score)||score<confidence_) continue;
                add((x+0.5f-dfl(b,0,bins,y,x))*sx,(y+0.5f-dfl(b,1,bins,y,x))*sy,
                    (x+0.5f+dfl(b,2,bins,y,x))*sx,(y+0.5f+dfl(b,3,bins,y,x))*sy,score);
            }
        }
    } else {
        std::cerr<<"[RKNN] unsupported output count="<<outputs.size()<<std::endl; return false;
    }
    nms(boxes,nms_,out);
    return true;
}
