// SPDX-License-Identifier: GPL-3.0-or-later
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL
#include "graphics.hpp"
#include <GL/gl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <sys/wait.h>

static constexpr int W=1280,H=800;
static constexpr float Radius=2.f,Angle=.8f,Aspect=float(W)/H;
static void xrcheck(XrResult result,const char* op){if(XR_FAILED(result))throw std::runtime_error(std::string(op)+": "+std::to_string(result));}
#define XR(call) xrcheck(call,#call)
struct Vec {float x,y,z;};
static Vec direction(XrQuaternionf q){
    return {-2*(q.x*q.z+q.w*q.y),-2*(q.y*q.z-q.w*q.x),-(1-2*(q.x*q.x+q.y*q.y))};
}
static bool hit(Vec o,Vec d,float& x,float& y){
    float a=d.x*d.x+d.z*d.z,b=2*(o.x*d.x+o.z*d.z),c=o.x*o.x+o.z*o.z-Radius*Radius;
    float disc=b*b-4*a*c;if(a<1e-8f||disc<0)return false;
    float roots[]={(-b-std::sqrt(disc))/(2*a),(-b+std::sqrt(disc))/(2*a)};
    for(float t:roots){if(t<=0)continue;float px=o.x+t*d.x,py=o.y+t*d.y,pz=o.z+t*d.z;
        float azimuth=std::atan2(px,-pz),u=azimuth/Angle+.5f,v=.5f-py/(Radius*Angle/Aspect);
        if(u>=0&&u<1&&v>=0&&v<1){x=u*W;y=v*H;return true;}}
    return false;
}
class Channel {
    int fd;
    QByteArray incoming,outgoing;
public:
    std::vector<uint8_t> pixels=std::vector<uint8_t>(size_t(W)*H*4,0);
    bool alive=true,haveImage=false;
    Channel(int socket):fd(socket){fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);}
    ~Channel(){close(fd);}
    void event(const char* type,float x,float y){
        QJsonObject obj{{"type",type},{"x",x},{"y",y},{"button",1}};
        auto data=QByteArray("I")+QJsonDocument(obj).toJson(QJsonDocument::Compact);
        if(outgoing.size()>65536){alive=false;return;}
        QByteArray header(4,'\0');qToBigEndian<quint32>(data.size(),header.data());outgoing+=header+data;
    }
    void ready(){QByteArray header(4,'\0');qToBigEndian<quint32>(1,header.data());outgoing+=header+"R";}
    void decode(){
        while(incoming.size()>=4){quint32 n=qFromBigEndian<quint32>(incoming.constData());
            if(n!=1+8+W*H*4){alive=false;return;}if(incoming.size()<n+4)return;
            auto data=incoming.constData()+4;
            if(data[0]!='F'||qFromBigEndian<quint32>(data+1)!=W||qFromBigEndian<quint32>(data+5)!=H){alive=false;return;}
            for(int y=0;y<H;++y)std::memcpy(pixels.data()+size_t(H-y-1)*W*4,data+9+size_t(y)*W*4,W*4);
            incoming.remove(0,n+4);haveImage=true;
        }
    }
    void pump(){
        while(!outgoing.isEmpty()){
            auto n=send(fd,outgoing.constData(),outgoing.size(),MSG_NOSIGNAL);
            if(n>0)outgoing.remove(0,n);else {if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)alive=false;break;}
        }
        char buf[65536];
        while(alive){auto n=recv(fd,buf,sizeof(buf),0);
            if(n>0){incoming.append(buf,n);decode();}
            else {if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))alive=false;break;}
        }
    }
};
struct Presenter {
    NativeGraphics graphics;
    XrInstance instance{};XrSession session{};XrSpace view{};XrSwapchain swapchain{};XrSystemId system{};
    XrActionSet actionSet{};XrAction aim{},select{};std::array<XrPath,2> hands{};std::array<XrSpace,2> aims{};
    std::array<bool,2> down{};std::array<XrVector2f,2> last{};
    std::vector<XrSwapchainImageOpenGLKHR> images;
    XrEnvironmentBlendMode blend{};
    bool running=false,quit=false;
    uint64_t frames=0,visible=0;
    XrPath path(const char* s){XrPath p{};XR(xrStringToPath(instance,s,&p));return p;}
    Presenter(){
        uint32_t count=0;XR(xrEnumerateInstanceExtensionProperties(nullptr,0,&count,nullptr));
        std::vector<XrExtensionProperties> extensions(count,{XR_TYPE_EXTENSION_PROPERTIES});XR(xrEnumerateInstanceExtensionProperties(nullptr,count,&count,extensions.data()));
        auto supported=[&](const char* name){return std::any_of(extensions.begin(),extensions.end(),[&](auto& x){return !strcmp(x.extensionName,name);});};
        std::vector<const char*> enabled={XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,XR_MNDX_EGL_ENABLE_EXTENSION_NAME,XR_EXTX_OVERLAY_EXTENSION_NAME,XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME};
        for(auto name:enabled)if(!supported(name))throw std::runtime_error(std::string("Required XR extension unavailable: ")+name);
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};strcpy(info.applicationInfo.applicationName,"LightXR greeter");
        info.applicationInfo.applicationVersion=1;info.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);info.enabledExtensionCount=enabled.size();info.enabledExtensionNames=enabled.data();XR(xrCreateInstance(&info,&instance));
        XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};get.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;XR(xrGetSystem(instance,&get,&system));
        PFN_xrGetOpenGLGraphicsRequirementsKHR requirementsFn{};XR(xrGetInstanceProcAddr(instance,"xrGetOpenGLGraphicsRequirementsKHR",reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFn)));
        XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};XR(requirementsFn(instance,system,&requirements));
        XrSessionCreateInfoOverlayEXTX overlay{XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX};overlay.sessionLayersPlacement=1000;
        XrGraphicsBindingEGLMNDX binding{XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};binding.next=&overlay;binding.getProcAddress=reinterpret_cast<PFN_xrEglGetProcAddressMNDX>(eglGetProcAddress);binding.display=graphics.display;binding.config=graphics.config;binding.context=graphics.context;
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};create.next=&binding;create.systemId=system;XR(xrCreateSession(instance,&create,&session));
        XrReferenceSpaceCreateInfo ref{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};ref.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;ref.poseInReferenceSpace.orientation.w=1;XR(xrCreateReferenceSpace(session,&ref,&view));
        XR(xrEnumerateEnvironmentBlendModes(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr));
        std::vector<XrEnvironmentBlendMode> modes(count);XR(xrEnumerateEnvironmentBlendModes(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,count,&count,modes.data()));
        blend=std::find(modes.begin(),modes.end(),XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)!=modes.end()?XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:modes.at(0);
        XR(xrEnumerateSwapchainFormats(session,0,&count,nullptr));std::vector<int64_t> formats(count);XR(xrEnumerateSwapchainFormats(session,count,&count,formats.data()));
        int64_t format=GL_SRGB8_ALPHA8;if(std::find(formats.begin(),formats.end(),format)==formats.end())format=GL_RGBA8;
        if(std::find(formats.begin(),formats.end(),format)==formats.end())throw std::runtime_error("No RGBA XR swapchain format");
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};sc.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;sc.format=format;sc.sampleCount=1;sc.width=W;sc.height=H;sc.faceCount=1;sc.arraySize=1;sc.mipCount=1;XR(xrCreateSwapchain(session,&sc,&swapchain));
        XR(xrEnumerateSwapchainImages(swapchain,0,&count,nullptr));images.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});XR(xrEnumerateSwapchainImages(swapchain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
        hands={path("/user/hand/left"),path("/user/hand/right")};
        XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};strcpy(set.actionSetName,"greeter");strcpy(set.localizedActionSetName,"LightXR greeter");set.priority=1000;XR(xrCreateActionSet(instance,&set,&actionSet));
        auto action=[&](const char* name,XrActionType type,XrAction& result){XrActionCreateInfo a{XR_TYPE_ACTION_CREATE_INFO};a.actionType=type;strcpy(a.actionName,name);strcpy(a.localizedActionName,name);a.countSubactionPaths=2;a.subactionPaths=hands.data();XR(xrCreateAction(actionSet,&a,&result));};
        action("aim",XR_ACTION_TYPE_POSE_INPUT,aim);action("select",XR_ACTION_TYPE_BOOLEAN_INPUT,select);
        for(auto profile:{"/interaction_profiles/khr/simple_controller","/interaction_profiles/oculus/touch_controller","/interaction_profiles/valve/index_controller","/interaction_profiles/htc/vive_controller"}) {
            std::vector<XrActionSuggestedBinding> bindings;
            for(auto hand:{std::string("/user/hand/left"),std::string("/user/hand/right")}){
                bindings.push_back({aim,path((hand+"/input/aim/pose").c_str())});
                bindings.push_back({select,path((hand+(std::string(profile).find("simple_controller")!=std::string::npos?"/input/select/click":"/input/trigger/value")).c_str())});
            }
            XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};suggestion.interactionProfile=path(profile);suggestion.countSuggestedBindings=bindings.size();suggestion.suggestedBindings=bindings.data();
            auto result=xrSuggestInteractionProfileBindings(instance,&suggestion);if(XR_FAILED(result))std::cerr<<"Optional controller profile unavailable: "<<profile<<'\n';
        }
        for(int i=0;i<2;++i){XrActionSpaceCreateInfo a{XR_TYPE_ACTION_SPACE_CREATE_INFO};a.action=aim;a.subactionPath=hands[i];a.poseInActionSpace.orientation.w=1;XR(xrCreateActionSpace(session,&a,&aims[i]));}
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};attach.countActionSets=1;attach.actionSets=&actionSet;XR(xrAttachSessionActionSets(session,&attach));
        std::cerr<<"LightXR cylinder ready: radius=2m distance=2m angle=0.8rad\n";
    }
    ~Presenter(){
        for(auto space:aims)if(space)xrDestroySpace(space);
        if(swapchain)xrDestroySwapchain(swapchain);
        if(view)xrDestroySpace(view);
        if(session)xrDestroySession(session);
        if(actionSet)xrDestroyActionSet(actionSet);
        if(instance)xrDestroyInstance(instance);
        std::cerr<<"LightXR frames="<<frames<<" visible="<<visible<<'\n';
    }
    void events(){XrEventDataBuffer data{XR_TYPE_EVENT_DATA_BUFFER};XrResult result;
        while((result=xrPollEvent(instance,&data))==XR_SUCCESS){
            if(data.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){auto& event=*reinterpret_cast<XrEventDataSessionStateChanged*>(&data);
                if(event.state==XR_SESSION_STATE_READY){XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;XR(xrBeginSession(session,&begin));running=true;}
                if(event.state==XR_SESSION_STATE_STOPPING){running=false;XR(xrEndSession(session));}
                if(event.state==XR_SESSION_STATE_EXITING||event.state==XR_SESSION_STATE_LOSS_PENDING)quit=true;
            }else if(data.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)quit=true;
            data={XR_TYPE_EVENT_DATA_BUFFER};
        }if(result!=XR_EVENT_UNAVAILABLE)xrcheck(result,"xrPollEvent");
    }
    void input(Channel& channel,XrTime time){
        XrActiveActionSet active{actionSet,XR_NULL_PATH};XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};sync.countActiveActionSets=1;sync.activeActionSets=&active;
        if(XR_FAILED(xrSyncActions(session,&sync)))return;
        for(int i=0;i<2;++i){
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};get.action=select;get.subactionPath=hands[i];XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};XR(xrGetActionStateBoolean(session,&get,&button));
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};XR(xrLocateSpace(aims[i],view,time,&location));float x=0,y=0;
            bool valid=(location.locationFlags&(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))==(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            bool inside=valid&&hit({location.pose.position.x,location.pose.position.y,location.pose.position.z},direction(location.pose.orientation),x,y);
            if(inside){last[i]={x,y};channel.event("move",x,y);}
            bool pressed=button.isActive&&button.currentState;
            if(pressed&&!down[i]&&inside){channel.event("down",x,y);down[i]=true;}
            if(!pressed&&down[i]){channel.event("up",last[i].x,last[i].y);down[i]=false;}
        }
    }
    void frame(Channel& channel){
        if(!running){usleep(10000);return;}
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState state{XR_TYPE_FRAME_STATE};XR(xrWaitFrame(session,&wait,&state));
        input(channel,state.predictedDisplayTime);channel.pump();
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};XR(xrBeginFrame(session,&begin));
        XrCompositionLayerCylinderKHR layer{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};layer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;layer.space=view;layer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        layer.subImage.swapchain=swapchain;layer.subImage.imageRect.extent={W,H};layer.pose.orientation.w=1;layer.radius=Radius;layer.centralAngle=Angle;layer.aspectRatio=Aspect;
        bool render=state.shouldRender&&channel.haveImage&&channel.alive;
        if(render){uint32_t index=0;XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};XR(xrAcquireSwapchainImage(swapchain,&acquire,&index));
            XrSwapchainImageWaitInfo ready{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};ready.timeout=XR_INFINITE_DURATION;XR(xrWaitSwapchainImage(swapchain,&ready));
            glBindTexture(GL_TEXTURE_2D,images[index].image);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,channel.pixels.data());glFlush();
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};XR(xrReleaseSwapchainImage(swapchain,&release));++visible;
        }
        const XrCompositionLayerBaseHeader* base=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=state.predictedDisplayTime;end.environmentBlendMode=blend;end.layerCount=render?1:0;end.layers=render?&base:nullptr;XR(xrEndFrame(session,&end));++frames;
    }
};
int main(int argc,char** argv)try{
    int fd=-1;float seconds=0;std::string report;
    for(int i=1;i<argc;++i){std::string arg(argv[i]);auto value=[&]{if(++i>=argc)throw std::runtime_error("Missing option value");return argv[i];};
        if(arg=="--bridge-fd")fd=std::stoi(value());else if(arg=="--seconds")seconds=std::stof(value());else if(arg=="--report")report=value();
        else if(arg=="--test-channel"){
            int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))throw std::runtime_error("Test socketpair");
            auto child=fork();if(child<0)throw std::runtime_error("Test fork");
            if(!child){close(pair[0]);
                QByteArray frame(13+W*H*4,'\0');qToBigEndian<quint32>(9+W*H*4,frame.data());frame[4]='F';qToBigEndian<quint32>(W,frame.data()+5);qToBigEndian<quint32>(H,frame.data()+9);
                for(int index=1;index<=4;++index){std::fill(frame.begin()+13,frame.end(),char(index));size_t sent=0;
                    while(sent<size_t(frame.size())){auto n=write(pair[1],frame.data()+sent,frame.size()-sent);if(n<=0)_exit(2);sent+=n;}}
                close(pair[1]);_exit(0);
            }
            close(pair[1]);Channel channel(pair[0]);auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(channel.alive&&std::chrono::steady_clock::now()<deadline){channel.pump();usleep(1000);}
            int status=0;waitpid(child,&status,0);
            if(status||!channel.haveImage||channel.pixels[0]!=4)throw std::runtime_error("Burst frames lost during incremental decode");
            std::cout<<"PASS: four full-size burst frames decoded incrementally without dropping the connection\n";return 0;
        }
        else if(arg=="--test-geometry"){
            float x=0,y=0;if(!hit({0,0,0},{0,0,-1},x,y)||std::abs(x-W*.5f)>0.01||std::abs(y-H*.5f)>0.01)throw std::runtime_error("Centre hit mismatch");
            if(hit({0,0,0},{0,0,1},x,y)||hit({0,0,0},{1,0,-1},x,y)||hit({0,0,0},{0,1,-1},x,y))throw std::runtime_error("Outside ray accepted");
            if(!hit({0,0,0},{std::sin(.2f),0,-std::cos(.2f)},x,y)||std::abs(x-W*.75f)>.01)throw std::runtime_error("Curved hit mismatch");
            std::cout<<"PASS: 2m cylinder, centre and curved pointer hits, back/edge rejection\n";return 0;
        }else throw std::runtime_error("Unknown option: "+arg);
    }
    if(fd<0)throw std::runtime_error("--bridge-fd is required (the seat broker owns the channel)");
    Channel channel(fd);Presenter xr;channel.ready();auto start=std::chrono::steady_clock::now();
    while(channel.alive&&!xr.quit){channel.pump();xr.events();xr.frame(channel);if(seconds>0&&std::chrono::duration<float>(std::chrono::steady_clock::now()-start).count()>seconds)break;}
    if(!report.empty()){std::ofstream out(report);out<<QJsonDocument(QJsonObject{{"frames",double(xr.frames)},{"visible",double(xr.visible)},{"radius_m",Radius},{"distance_m",Radius},{"curved",true}}).toJson().constData();}
    return 0;
}catch(const std::exception& e){std::cerr<<"LightXR presenter: "<<e.what()<<'\n';return 1;}
