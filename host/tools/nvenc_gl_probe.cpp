// SPDX-License-Identifier: GPL-3.0-only
/** @file nvenc_gl_probe.cpp
 * @brief Offline matched native-NVENC CUDA versus direct-OpenGL input experiment.
 * No game capture, live service configuration, or device privileges are changed.
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <cuda.h>
#include <cudaGL.h>
#include <cudaEGL.h>
#include <nvEncodeAPI.h>
#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef SUNSHINE_TRACE_NVTX
#include <nvtx3/nvToolsExt.h>
/** @brief Optional diagnostic range; excluded from normal benchmark builds. */
struct TraceRange {
  bool active=false;
  void begin(const char *name) {nvtxRangePushA(name);active=true;}
  ~TraceRange(){if(active)nvtxRangePop();}
};
#endif

using Clock = std::chrono::steady_clock;
static NV_ENC_SPLIT_ENCODE_MODE split_policy=NV_ENC_SPLIT_TWO_FORCED_MODE; ///< Probe-only session policy.
static bool stable_source=false; ///< Isolate encoding with a preinitialized, unchanged GPU surface.
static bool rgb_input=false; ///< Source-only experiment: let the encoder accept packed RGBA input.
static bool matched_gray_pattern=false; ///< Use matched gray content for the RGB/NV12 comparison.
static unsigned encode_policy=0; ///< Probe-only: default CBR, minimum CU16/CU32, or constant QP28.
static bool output_in_vram=false; ///< Probe-only documented default-stream GPU bitstream readback.
static bool disable_filler_data=false; ///< Controlled override of preset padding; required for GPU output.
static bool explicit_io_stream=true; ///< Legacy/default-stream control is restricted to ready static inputs.
static unsigned pace_fps=0; ///< Optional real-time cadence; sleeps occur before the measured source span.
static unsigned green_sm_count=0; ///< Optional owned green context; no game or global driver configuration.
static std::string native_codec="hevc", context_wait="blocking";
static std::string bitstream_wait="blocking"; ///< Benchmark-only completion policy; never changes a game or service.
static bool h264_cavlc=false; ///< Probe-only entropy policy; normal decoding must still pass.
static unsigned context_flags() {
  return context_wait=="spin" ? CU_CTX_SCHED_SPIN : context_wait=="yield" ? CU_CTX_SCHED_YIELD :
         context_wait=="auto" ? CU_CTX_SCHED_AUTO : CU_CTX_SCHED_BLOCKING_SYNC;
}
static const char *codec_name(bool av1) {return av1 ? "av1" : native_codec.c_str();}
static const char *policy_name(){return encode_policy==1 ? "cbr-min-cu16" : encode_policy==2 ? "cbr-min-cu32" : encode_policy==3 ? "constqp28" : "cbr-default";}
/** @brief Map an 8-bit gray code to limited-range luma, independent of matrix coefficients. */
static unsigned char limited_gray(unsigned gray) { return 16+(219*gray+127)/255; }
/** @brief Raise a labeled error without treating a failed operation as a sample. */
static void require(bool ok, const char *label) { if (!ok) throw std::runtime_error(label); }
/** @brief Validate a CUDA driver call. */
static void cu(CUresult result) {
  if (result != CUDA_SUCCESS) { const char *name=nullptr; cuGetErrorName(result,&name); throw std::runtime_error(std::string("CUDA: ")+(name ? name : "unknown")); }
}
/** @brief Validate an NVENC call; NEED_MORE_INPUT is not completed output. */
static void nv(NVENCSTATUS result) {
  if (result != NV_ENC_SUCCESS) throw std::runtime_error("NVENC status " + std::to_string(result));
}
/** @brief Resolve the optional EGL fence bridge; a successful query may still return null. */
static auto egl_event_bridge() {
  using Function=decltype(&cuEventCreateFromEGLSync);
  static Function fn=[](){
    void *pointer=nullptr; CUdriverProcAddressQueryResult status;
    cu(cuInit(0));
    auto result=cuGetProcAddress("cuEventCreateFromEGLSync",&pointer,12000,CU_GET_PROC_ADDRESS_DEFAULT,&status);
    return result==CUDA_SUCCESS && status==CU_GET_PROC_ADDRESS_SUCCESS ? reinterpret_cast<Function>(pointer) : nullptr;
  }();
  return fn;
}
/** @brief Report a wall duration in microseconds. */
static double elapsed(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::micro>(b-a).count();
}
/** @brief Read current graphics/video clocks without changing power policy. */
struct ClockTelemetry {
  void *library=nullptr,*device=nullptr;
  bool initialized=false;
  int (*clock_info)(void*,int,unsigned*)=nullptr;
  int (*shutdown)()=nullptr;
  ClockTelemetry() {
    library=dlopen("libnvidia-ml.so.1",RTLD_NOW|RTLD_LOCAL);if(!library)return;
    auto init=reinterpret_cast<int(*)()>(dlsym(library,"nvmlInit_v2"));
    auto handle=reinterpret_cast<int(*)(unsigned,void**)>(dlsym(library,"nvmlDeviceGetHandleByIndex_v2"));
    clock_info=reinterpret_cast<decltype(clock_info)>(dlsym(library,"nvmlDeviceGetClockInfo"));
    shutdown=reinterpret_cast<decltype(shutdown)>(dlsym(library,"nvmlShutdown"));
    if(init && handle && clock_info && shutdown && init()==0) { initialized=true;handle(0,&device); }
  }
  ~ClockTelemetry() { if(initialized)shutdown();if(library)dlclose(library); }
  unsigned mhz(int kind)const { unsigned value=0;if(device)clock_info(device,kind,&value);return value; }
};
static ClockTelemetry &clocks() { static ClockTelemetry instance;return instance; }
#ifdef SUNSHINE_MOTION_PROBE
#include "pixel_motion.h"
#endif
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
#include "vulkan_shared_surface.h"
static std::filesystem::path shared_shader;
#endif
/** @brief Close optional fixture files on every exit. */
struct FileCloser { void operator()(FILE *file) const { std::fclose(file); } };
/** @brief Own one isolated native encoder experiment, including partial initialization. */
struct Probe {
  NV_ENCODE_API_FUNCTION_LIST &api;
  void *encoder = nullptr;
  NV_ENC_REGISTERED_PTR registered = nullptr;
  NV_ENC_REGISTERED_PTR copy_registered = nullptr; ///< Optional paired control in the same encoder session.
  NV_ENC_INPUT_PTR mapped = nullptr;
  NV_ENC_OUTPUT_PTR bitstream = nullptr;
  NV_ENC_REGISTERED_PTR output_registered = nullptr;
  NV_ENC_INPUT_PTR output_mapped = nullptr;
  CUdeviceptr output_allocation = 0;
  size_t output_capacity = 0;
  bool locked = false;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext glcontext = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  GLuint texture = 0;
  CUcontext cudacontext = nullptr;
  CUgreenCtx greencontext = nullptr;
  CUstream stream = nullptr;
  CUgraphicsResource graphics_resource = nullptr;
  bool graphics_mapped = false;
  EGLImage source_image = EGL_NO_IMAGE;
  CUarray persistent_egl_array = nullptr; ///< EGL registration keeps this array valid until unregister.
  EGLSync ready_sync = EGL_NO_SYNC;
  CUevent ready_event = nullptr;
  CUdeviceptr allocation = 0;
  CUarray owned_array = nullptr; ///< Optional codec-native or generic persistent CUDA input.
  size_t pitch = 0;
  NV_ENC_INPUT_RESOURCE_OPENGL_TEX glresource {};
  int granted_priority = 0;
  NV_ENC_BUFFER_FORMAT input_format = NV_ENC_BUFFER_FORMAT_NV12;
  unsigned api_split_setting = 0; ///< Exact value passed to initialization, distinct from requested policy.
  inline static unsigned width = 1920, height = 1080;
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
  std::unique_ptr<VulkanSharedSurface> shared;
  std::unique_ptr<GlCudaSemaphoreBridge> semaphore_bridge;
#endif
  explicit Probe(NV_ENCODE_API_FUNCTION_LIST &functions): api(functions) {}
  /** @brief Include the failing operation and driver's own diagnostic. */
  void check_nv(NVENCSTATUS status,const char *operation) {
    if(status==NV_ENC_SUCCESS)return;
    const char *detail=encoder && api.nvEncGetLastErrorString ? api.nvEncGetLastErrorString(encoder) : nullptr;
    throw std::runtime_error(std::string(operation)+" status "+std::to_string(status)+": "+(detail ? detail : "no driver detail"));
  }
  /** @brief Keep mapped-array ownership distinct from the array-to-linear copy. */
  static const char *backend(bool opengl, int interop) {
    return interop==11 ? "egl-semaphore" : interop==1 ? "gl-map" : interop==2 ? "egl-event" : interop==3 ? "gl-array" : interop==4 ? "gl-array-copy" : interop==5 ? "vk-shared-copy" : interop==6 ? "vk-shared-array" : interop==7 ? "vk-shared-paired" : interop==8 ? "cuda-native-nv12" : interop==9 ? "cuda-generic-array" : interop==10 ? "cuda-native-paired" : opengl ? "opengl" : "cuda";
  }
  /** @brief Register only while the underlying input allocation remains valid. */
  void register_input(NV_ENC_INPUT_RESOURCE_TYPE type, void *resource) {
    NV_ENC_REGISTER_RESOURCE reg {}; reg.version=NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType=type; reg.resourceToRegister=resource;
    reg.width=width; reg.height=height; reg.pitch=static_cast<unsigned>(pitch); reg.bufferFormat=input_format;
    reg.bufferUsage=NV_ENC_INPUT_IMAGE;
    auto status=api.nvEncRegisterResource(encoder,&reg);
    if(status!=NV_ENC_SUCCESS) {
      const char *detail=api.nvEncGetLastErrorString ? api.nvEncGetLastErrorString(encoder) : nullptr;
      throw std::runtime_error("NVENC input registration type "+std::to_string(type)+" status "+std::to_string(status)+": "+(detail ? detail : "no driver detail"));
    }
    registered=reg.registeredResource;
  }
  /** @brief Release encoder resources before their producing GPU contexts. */
  ~Probe() {
    if (encoder) {
      if (locked) api.nvEncUnlockBitstream(encoder, bitstream);
      if (mapped) api.nvEncUnmapInputResource(encoder, mapped);
      if (output_mapped) api.nvEncUnmapInputResource(encoder, output_mapped);
      if (output_registered) api.nvEncUnregisterResource(encoder, output_registered);
      if (registered) api.nvEncUnregisterResource(encoder, registered);
      if (copy_registered) api.nvEncUnregisterResource(encoder, copy_registered);
      if (bitstream && !output_allocation) api.nvEncDestroyBitstreamBuffer(encoder, bitstream);
      api.nvEncDestroyEncoder(encoder);
    }
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
    semaphore_bridge.reset();
    shared.reset(); // Imported CUDA views must die while their CUDA context is current.
#endif
    if (graphics_mapped) cuGraphicsUnmapResources(1, &graphics_resource, stream);
    if (cudacontext && graphics_resource) cuCtxSynchronize();
    if (ready_event) cuEventDestroy(ready_event);
    if (ready_sync != EGL_NO_SYNC) eglDestroySync(display, ready_sync);
    if (graphics_resource) cuGraphicsUnregisterResource(graphics_resource);
    if (source_image != EGL_NO_IMAGE) eglDestroyImage(display, source_image);
    if (allocation) cuMemFree(allocation);
    if (output_allocation) cuMemFree(output_allocation);
    if (owned_array) cuArrayDestroy(owned_array);
    if (stream) cuStreamDestroy(stream);
    if (greencontext) cuGreenCtxDestroy(greencontext);
    else if (cudacontext) cuCtxDestroy(cudacontext);
    if (texture) glDeleteTextures(1, &texture);
    if (display != EGL_NO_DISPLAY) {
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
      if (glcontext != EGL_NO_CONTEXT) eglDestroyContext(display, glcontext);
      eglTerminate(display);
    }
  }
  /** @brief Create an offscreen NVIDIA context; no desktop/window capture is needed. */
  void init_gl(bool high, GLenum target=GL_TEXTURE_RECTANGLE) {
    auto devices_fn = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    auto platform_fn = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    require(devices_fn && platform_fn, "EGL device enumeration unavailable");
    EGLDeviceEXT devices[16]; EGLint count = 0;
    require(devices_fn(16, devices, &count) && count > 0, "No EGL devices");
    display = platform_fn(EGL_PLATFORM_DEVICE_EXT, devices[0], nullptr);
    require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "EGL initialize failed");
    require(eglBindAPI(EGL_OPENGL_API), "EGL OpenGL unavailable");
    EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig config; EGLint configs = 0;
    require(eglChooseConfig(display, config_attrs, &config, 1, &configs) && configs, "EGL config failed");
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    bool priority = extensions && std::strstr(extensions, "EGL_IMG_context_priority");
    require(!high || priority, "High context priority extension unavailable");
    std::vector<EGLint> attrs {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5};
    if (priority) { attrs.push_back(EGL_CONTEXT_PRIORITY_LEVEL_IMG); attrs.push_back(high ? EGL_CONTEXT_PRIORITY_HIGH_IMG : EGL_CONTEXT_PRIORITY_MEDIUM_IMG); }
    attrs.push_back(EGL_NONE);
    glcontext = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs.data());
    require(glcontext != EGL_NO_CONTEXT, "OpenGL context creation failed");
    EGLint pbuffer[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, pbuffer);
    require(surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, glcontext), "EGL make-current failed");
    require(std::strstr(reinterpret_cast<const char *>(glGetString(GL_VENDOR)), "NVIDIA"), "Probe requires NVIDIA OpenGL");
    if (priority) eglQueryContext(display, glcontext, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &granted_priority);
    glGenTextures(1, &texture); glBindTexture(target, texture);
    glTexParameteri(target,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(target,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    if(target==GL_TEXTURE_2D)glTexParameteri(target,GL_TEXTURE_MAX_LEVEL,0);
  }
  /** @brief Configure identical low-delay hardware policy for either input backend. */
  void init(bool opengl, bool av1, bool high, int interop=0) {
    require(!opengl || !green_sm_count,"Green context experiment is CUDA-only");
    require(!rgb_input || !interop || interop==3 || interop==4,"RGB interop only supports direct mapped arrays or their copy control");
    require(explicit_io_stream || stable_source,"Legacy I/O stream control requires already-complete static input");
    require(!output_in_vram || (!opengl && !explicit_io_stream && stable_source && split_policy==NV_ENC_SPLIT_DISABLE_MODE),
            "GPU output requires a ready CUDA source, default I/O stream and disabled split encoding");
    input_format=rgb_input ? NV_ENC_BUFFER_FORMAT_ABGR : NV_ENC_BUFFER_FORMAT_NV12;
    unsigned row_bytes=width*(rgb_input ? 4 : 1);
    unsigned storage_height=rgb_input ? height : height*3/2;
    std::vector<unsigned char> input(size_t(row_bytes)*storage_height,128);
    for (unsigned y=0; y<height; ++y) for (unsigned x=0; x<width; ++x) {
      unsigned gray=(x/8+y/8)%256;
      if(rgb_input) {
        size_t offset=(size_t(y)*width+x)*4;
        input[offset]=input[offset+1]=input[offset+2]=gray;input[offset+3]=255;
      } else input[y*width+x]=matched_gray_pattern ? limited_gray(gray) : static_cast<unsigned char>(16+((x/8+y/8)%220));
    }
    if (opengl || (interop && interop<5) || interop==11) {
      GLenum target=interop ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE;
      init_gl(high,target);
      glTexImage2D(target,0,rgb_input ? GL_RGBA8 : GL_R8,width,storage_height,0,rgb_input ? GL_RGBA : GL_RED,GL_UNSIGNED_BYTE,input.data());
      glFinish(); require(glGetError()==GL_NO_ERROR, "GL input initialization failed");
      glresource.texture = texture; glresource.target = target; pitch = row_bytes;
    }
    if (!opengl) {
      CUdevice device; cu(cuInit(0)); cu(cuDeviceGet(&device, 0));
      if(green_sm_count) {
        CUdevResource all {},part {},remainder {};cu(cuDeviceGetDevResource(device,&all,CU_DEV_RESOURCE_TYPE_SM));
        require(green_sm_count<=all.sm.smCount && green_sm_count%all.sm.smCoscheduledAlignment==0,"Green SM request violates device count/alignment");
        CU_DEV_SM_RESOURCE_GROUP_PARAMS grouping {};grouping.smCount=green_sm_count;
        cu(cuDevSmResourceSplit(&part,1,&all,&remainder,0,&grouping));
        CUdevResource resources[2] {part,{}};resources[1].type=CU_DEV_RESOURCE_TYPE_WORKQUEUE_CONFIG;
        resources[1].wqConfig.device=device;resources[1].wqConfig.sharingScope=CU_WORKQUEUE_SCOPE_GREEN_CTX_BALANCED;resources[1].wqConfig.wqConcurrencyLimit=2;
        CUdevResourceDesc descriptor=nullptr;cu(cuDevResourceGenerateDesc(&descriptor,resources,2));
        cu(cuGreenCtxCreate(&greencontext,descriptor,device,CU_GREEN_CTX_DEFAULT_STREAM));
        cu(cuCtxFromGreenCtx(&cudacontext,greencontext));cu(cuCtxSetCurrent(cudacontext));
        CUdevResource granted {};cu(cuGreenCtxGetDevResource(greencontext,&granted,CU_DEV_RESOURCE_TYPE_SM));
        require(granted.sm.smCount==green_sm_count,"Green context SM grant differs from request");
        cu(cuGreenCtxStreamCreate(&stream,greencontext,CU_STREAM_NON_BLOCKING,0));
      } else {
      #if CUDA_VERSION >= 13000
      CUctxCreateParams params {};
      cu(cuCtxCreate(&cudacontext, &params, context_flags(), device));
#else
      cu(cuCtxCreate(&cudacontext, context_flags(), device));
#endif
      cu(cuStreamCreate(&stream, CU_STREAM_DEFAULT));
      }
      if (interop!=3 && interop!=6 && (interop<8 || interop==10 || interop==11)) {
        cu(cuMemAllocPitch(&allocation, &pitch, row_bytes, storage_height, 16));
        CUDA_MEMCPY2D copy {}; copy.srcMemoryType=CU_MEMORYTYPE_HOST; copy.srcHost=input.data(); copy.srcPitch=row_bytes;
        copy.dstMemoryType=CU_MEMORYTYPE_DEVICE; copy.dstDevice=allocation; copy.dstPitch=pitch;
        copy.WidthInBytes=row_bytes; copy.Height=storage_height; cu(cuMemcpy2D(&copy));
      }
      if(interop>=8 && interop<=10) {
        require(!rgb_input && stable_source,"Owned array probe requires static NV12 input");
        bool native=interop==8 || interop==10;
        CUDA_ARRAY3D_DESCRIPTOR desc {};desc.Width=width;desc.Height=native ? height : storage_height;
        // Multi-planar NV12 counts Y/U/V logically; generic R8 counts one packed channel.
        desc.Format=native ? CU_AD_FORMAT_NV12 : CU_AD_FORMAT_UNSIGNED_INT8;desc.NumChannels=native ? 3 : 1;
        desc.Flags=CUDA_ARRAY3D_SURFACE_LDST|(native ? CUDA_ARRAY3D_VIDEO_ENCODE_DECODE : 0);
        cu(cuArray3DCreate(&owned_array,&desc));if(interop!=10)pitch=width;
        CUDA_MEMCPY2D copy {};copy.srcMemoryType=CU_MEMORYTYPE_HOST;copy.srcPitch=width;copy.dstMemoryType=CU_MEMORYTYPE_ARRAY;copy.WidthInBytes=width;
        if(native) {
          CUarray y,uv;cu(cuArrayGetPlane(&y,owned_array,0));cu(cuArrayGetPlane(&uv,owned_array,1));
          copy.srcHost=input.data();copy.dstArray=y;copy.Height=height;cu(cuMemcpy2D(&copy));
          copy.srcHost=input.data()+size_t(width)*height;copy.dstArray=uv;copy.Height=height/2;cu(cuMemcpy2D(&copy));
          std::vector<unsigned char> checked(input.size());
          CUDA_MEMCPY2D read {};read.srcMemoryType=CU_MEMORYTYPE_ARRAY;read.dstMemoryType=CU_MEMORYTYPE_HOST;read.dstPitch=width;read.WidthInBytes=width;
          read.srcArray=y;read.dstHost=checked.data();read.Height=height;cu(cuMemcpy2D(&read));
          read.srcArray=uv;read.dstHost=checked.data()+size_t(width)*height;read.Height=height/2;cu(cuMemcpy2D(&read));
          require(checked==input,"Codec-native CUDA plane readback does not match initialization");
        } else {copy.srcHost=input.data();copy.dstArray=owned_array;copy.Height=storage_height;cu(cuMemcpy2D(&copy));}
      }
    }
    if (interop == 1 || interop == 3 || interop == 4) {
      unsigned flags=interop==1 ? CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY : CU_GRAPHICS_REGISTER_FLAGS_SURFACE_LDST;
      cu(cuGraphicsGLRegisterImage(&graphics_resource, texture, GL_TEXTURE_2D, flags));
    } else if (interop == 2 || interop == 11) {
      const EGLAttrib attrs[]={EGL_IMAGE_PRESERVED,EGL_TRUE,EGL_NONE};
      source_image=eglCreateImage(display,glcontext,EGL_GL_TEXTURE_2D,reinterpret_cast<EGLClientBuffer>(uintptr_t(texture)),attrs);
      require(source_image!=EGL_NO_IMAGE,"EGL source image creation failed");
      cu(cuGraphicsEGLRegisterImage(&graphics_resource,source_image,CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY));
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
      if(interop==11){
        CUeglFrame source{};cu(cuGraphicsResourceGetMappedEglFrame(&source,graphics_resource,0,0));
        require(source.frameType==CU_EGL_FRAME_TYPE_ARRAY && source.planeCount==1 && source.width==width && source.height==height*3/2 && source.numChannels==1,"Unexpected persistent EGL source layout");
        persistent_egl_array=source.frame.pArray[0];
        semaphore_bridge=std::make_unique<GlCudaSemaphoreBridge>();semaphore_bridge->init();
      }
#endif
    }
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
    if (interop>=5 && interop<=7) {
      require(!shared_shader.empty(),"Shared Vulkan mode requires --shared-shader PATH");
      shared=std::make_unique<VulkanSharedSurface>();shared->init(width,height,shared_shader);
      if(interop==6) pitch=width;
    }
#endif
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open {}; open.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType=opengl ? NV_ENC_DEVICE_TYPE_OPENGL : NV_ENC_DEVICE_TYPE_CUDA;
    open.device=opengl ? nullptr : cudacontext; open.apiVersion=NVENCAPI_VERSION;
    auto opened=api.nvEncOpenEncodeSessionEx(&open,&encoder);
    if(opened!=NV_ENC_SUCCESS) throw std::runtime_error("NVENC session open status "+std::to_string(opened));
    bool h264=!av1 && native_codec=="h264";
    GUID codec=av1 ? NV_ENC_CODEC_AV1_GUID : h264 ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
    if(rgb_input) {
      uint32_t count=0,written=0;nv(api.nvEncGetInputFormatCount(encoder,codec,&count));
      std::vector<NV_ENC_BUFFER_FORMAT> formats(count);
      require(count>0,"No encoder input formats");
      nv(api.nvEncGetInputFormats(encoder,codec,formats.data(),count,&written));
      require(written<=count && std::find(formats.begin(),formats.begin()+written,input_format)!=formats.begin()+written,"ABGR input unsupported by this encoder session");
    }
    NV_ENC_PRESET_CONFIG preset {}; preset.version=NV_ENC_PRESET_CONFIG_VER; preset.presetCfg.version=NV_ENC_CONFIG_VER;
    check_nv(api.nvEncGetEncodePresetConfigEx(encoder,codec,NV_ENC_PRESET_P1_GUID,NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,&preset),"NVENC preset query");
    auto config=preset.presetCfg; config.gopLength=NVENC_INFINITE_GOPLENGTH; config.frameIntervalP=1;
    config.rcParams.rateControlMode=NV_ENC_PARAMS_RC_CBR; config.rcParams.averageBitRate=23000000;
    config.rcParams.maxBitRate=23000000; config.rcParams.vbvBufferSize=23000000/120;
    config.rcParams.vbvInitialDelay=config.rcParams.vbvBufferSize; config.rcParams.multiPass=NV_ENC_MULTI_PASS_DISABLED;
    config.rcParams.enableLookahead=0; config.rcParams.enableAQ=0; config.rcParams.enableTemporalAQ=0;
    config.rcParams.zeroReorderDelay=1;
    if(encode_policy==3){config.rcParams.rateControlMode=NV_ENC_PARAMS_RC_CONSTQP;config.rcParams.constQP={28,28,28};config.rcParams.averageBitRate=0;config.rcParams.maxBitRate=0;config.rcParams.vbvBufferSize=0;config.rcParams.vbvInitialDelay=0;}
    if (av1) { config.encodeCodecConfig.av1Config.chromaFormatIDC=1; config.encodeCodecConfig.av1Config.idrPeriod=NVENC_INFINITE_GOPLENGTH; }
    else if(h264) {
      config.encodeCodecConfig.h264Config.chromaFormatIDC=1;
      config.encodeCodecConfig.h264Config.idrPeriod=NVENC_INFINITE_GOPLENGTH;
      config.encodeCodecConfig.h264Config.entropyCodingMode=h264_cavlc ? NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC : NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    }
    else { config.encodeCodecConfig.hevcConfig.chromaFormatIDC=1; config.encodeCodecConfig.hevcConfig.idrPeriod=NVENC_INFINITE_GOPLENGTH; }
    if(disable_filler_data) {
      if(av1)config.encodeCodecConfig.av1Config.enableBitstreamPadding=0;
      else if(h264)config.encodeCodecConfig.h264Config.enableFillerDataInsertion=0;
      else config.encodeCodecConfig.hevcConfig.enableFillerDataInsertion=0;
    }
    if(!av1&&(encode_policy==1||encode_policy==2)){config.encodeCodecConfig.hevcConfig.minCUSize=encode_policy==1 ? NV_ENC_HEVC_CUSIZE_16x16 : NV_ENC_HEVC_CUSIZE_32x32;config.encodeCodecConfig.hevcConfig.maxCUSize=NV_ENC_HEVC_CUSIZE_32x32;}
    NV_ENC_INITIALIZE_PARAMS init {}; init.version=NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID=codec; init.presetGUID=NV_ENC_PRESET_P1_GUID; init.tuningInfo=NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth=init.darWidth=width; init.encodeHeight=init.darHeight=height;
    init.maxEncodeWidth=width; init.maxEncodeHeight=height; init.frameRateNum=120; init.frameRateDen=1;
    init.enablePTD=1; init.splitEncodeMode=h264 ? NV_ENC_SPLIT_DISABLE_MODE : split_policy; init.encodeConfig=&config;
    init.enableOutputInVidmem=output_in_vram;
    // GPU output itself disables split encoding. Let the driver apply that
    // documented constraint through the zero-valued automatic setting.
    if(output_in_vram)init.splitEncodeMode=NV_ENC_SPLIT_AUTO_MODE;
#ifdef SUNSHINE_MOTION_PROBE
    if(motion_hints_enabled) {
      require(!av1,"Initial motion-hint experiment supports HEVC only");
      init.enableExternalMEHints=1;init.maxMEHintCountsPerBlock[0].numCandsPerBlk16x16=1;
    }
#endif
    api_split_setting=init.splitEncodeMode;
    check_nv(api.nvEncInitializeEncoder(encoder,&init),"NVENC initialize");
    if (!opengl && explicit_io_stream) check_nv(api.nvEncSetIOCudaStreams(encoder,&stream,&stream),"NVENC I/O streams");
    if (interop!=3) {
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
      if (interop==6 || interop==7) {
        if(interop==7) {
          register_input(NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR,reinterpret_cast<void *>(allocation));
          copy_registered=registered;registered=nullptr;
        }
        size_t linear_pitch=pitch;pitch=width;
        register_input(NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY,static_cast<void *>(shared->array));pitch=linear_pitch;
      }
      else
#endif
      if(interop>=8 && interop<=10) {
        if(interop==10) {
          register_input(NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR,reinterpret_cast<void *>(allocation));
          copy_registered=registered;registered=nullptr;
        }
        size_t linear_pitch=pitch;pitch=width;
        register_input(NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY,static_cast<void *>(owned_array));pitch=linear_pitch;
      }
      else register_input(opengl ? NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX : NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR,
                          opengl ? static_cast<void *>(&glresource) : reinterpret_cast<void *>(allocation));
    }
    if(output_in_vram) {
      output_capacity=2*size_t(width)*height*3/2+sizeof(NV_ENC_ENCODE_OUT_PARAMS);
      cu(cuMemAlloc(&output_allocation,output_capacity));
      NV_ENC_REGISTER_RESOURCE output {};output.version=NV_ENC_REGISTER_RESOURCE_VER;
      output.resourceType=NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
      output.resourceToRegister=reinterpret_cast<void*>(output_allocation);
      output.width=static_cast<unsigned>(output_capacity);output.height=1;output.pitch=static_cast<unsigned>(output_capacity);
      output.bufferFormat=NV_ENC_BUFFER_FORMAT_U8;output.bufferUsage=NV_ENC_OUTPUT_BITSTREAM;
      check_nv(api.nvEncRegisterResource(encoder,&output),"NVENC register GPU output");output_registered=output.registeredResource;
    } else {
      NV_ENC_CREATE_BITSTREAM_BUFFER buffer {}; buffer.version=NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
      check_nv(api.nvEncCreateBitstreamBuffer(encoder,&buffer),"NVENC create bitstream");bitstream=buffer.bitstreamBuffer;
    }
  }
  /** @brief Measure completed packets with explicit input-readiness accounting. */
  void run(bool opengl, bool av1, bool high, int round, const std::filesystem::path &fixtures, bool pipelined=false, int interop=0) {
    init(opengl, av1, high, interop);
    unsigned video_clock_before=clocks().mhz(3),graphics_clock_before=clocks().mhz(0);
    std::vector<double> readiness, encode, total, cycle; unsigned long long bytes=0;
    std::vector<double> direct_total,copy_total,direct_cycle,copy_cycle;
#ifdef SUNSHINE_TRACE_NVTX
    TraceRange measured_range;
#endif
    std::vector<unsigned char> packet;
    std::unique_ptr<FILE,FileCloser> fixture;
    if (!fixtures.empty()) {
      auto name=fixtures/(std::to_string(width)+"x"+std::to_string(height)+"-"+policy_name()+"-"+backend(opengl,interop)+(rgb_input ? "-rgba" : "-nv12")+(high ? "-high" : "-normal")+(pipelined ? "-async" : "-sync")+"-split"+std::to_string(split_policy)+(stable_source ? "-static" : "-moving")+(output_in_vram?"-gpu-output":"")+"-"+codec_name(av1)+"-"+std::to_string(round)+(av1 ? ".obu" : native_codec=="h264" ? ".h264" : ".hevc"));
      fixture.reset(std::fopen(name.c_str(), "wb")); require(bool(fixture), "Fixture open failed");
    }
    auto thread_cpu=[](){timespec time{};require(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&time)==0,"Thread CPU clock unavailable");return time.tv_sec*1e6+time.tv_nsec/1e3;};
    double cpu_start=thread_cpu();unsigned long long busy_responses=0;
    auto cadence_start=Clock::now();
    for (unsigned frame=0; frame<120; ++frame) {
      if(pace_fps) std::this_thread::sleep_until(cadence_start+std::chrono::nanoseconds(uint64_t(frame)*1000000000/pace_fps));
#ifdef SUNSHINE_TRACE_NVTX
      if(frame==20) measured_range.begin(backend(opengl,interop));
#endif
      auto begin=Clock::now(); unsigned row=(frame*8)%(height-64);unsigned gray=(frame*13)%256;
      bool paired=interop==7 || interop==10;
      bool shared_copy=interop==5 || (paired && (frame+unsigned(round))%2==0);
      unsigned char value=matched_gray_pattern ? limited_gray(gray) : static_cast<unsigned char>(16+(frame*13)%220);
      unsigned char rgba[]={static_cast<unsigned char>(gray),static_cast<unsigned char>(gray),static_cast<unsigned char>(gray),255};
      if (stable_source) {
        // Initialized synchronously before timing; no new GPU producer work.
      }
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
      else if (interop>=5 && interop<=7) {
        shared->produce(frame,stream);
        if(shared_copy) {
          CUDA_MEMCPY2D copy {};copy.srcMemoryType=CU_MEMORYTYPE_ARRAY;copy.srcArray=shared->array;
          copy.dstMemoryType=CU_MEMORYTYPE_DEVICE;copy.dstDevice=allocation;copy.dstPitch=pitch;
          copy.WidthInBytes=width;copy.Height=height*3/2;cu(cuMemcpy2DAsync(&copy,stream));
        }
      }
#endif
      else if (interop) {
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
        if(interop==11)semaphore_bridge->before_write(frame,texture);
#endif
        glClearTexSubImage(texture,0,0,row,0,width,64,1,rgb_input?GL_RGBA:GL_RED,GL_UNSIGNED_BYTE,rgb_input?rgba:&value);
        CUarray array=nullptr;
        if (interop != 2 && interop != 11) {
          glFlush();
          cu(cuGraphicsMapResources(1,&graphics_resource,stream)); graphics_mapped=true;
          cu(cuGraphicsSubResourceGetMappedArray(&array,graphics_resource,0,0));
        } else if(interop==11) {
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
          semaphore_bridge->ready_for_cuda(texture,stream);
          array=persistent_egl_array;
#else
          throw std::runtime_error("External semaphore bridge was not compiled");
#endif
        } else {
          ready_sync=eglCreateSync(display,EGL_SYNC_FENCE,nullptr);
          require(ready_sync!=EGL_NO_SYNC,"EGL source-ready fence creation failed"); glFlush();
          auto bridge=egl_event_bridge();require(bridge,"CUDA EGL fence bridge unavailable");
          cu(bridge(&ready_event,ready_sync,CU_EVENT_DEFAULT));
          cu(cuStreamWaitEvent(stream,ready_event,0));
          CUeglFrame source {}; cu(cuGraphicsResourceGetMappedEglFrame(&source,graphics_resource,0,0));
          require(source.frameType==CU_EGL_FRAME_TYPE_ARRAY,"EGL source is not a CUDA array");array=source.frame.pArray[0];
        }
        if (interop==3) {
          CUDA_ARRAY3D_DESCRIPTOR descriptor {}; cu(cuArray3DGetDescriptor(&descriptor,array));
          require(descriptor.Width==width && descriptor.Height==(rgb_input?height:height*3/2) && descriptor.Depth==0 &&
                  descriptor.Format==CU_AD_FORMAT_UNSIGNED_INT8 && descriptor.NumChannels==(rgb_input?4:1) &&
                  (descriptor.Flags&CUDA_ARRAY3D_SURFACE_LDST),"Mapped array does not satisfy NVENC input contract");
          pitch=descriptor.Width*descriptor.NumChannels;
          // A mapped CUarray can change each frame. Never cache its registration across unmap.
          register_input(NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY,static_cast<void *>(array));
        } else {
          CUDA_MEMCPY2D copy {};copy.srcMemoryType=CU_MEMORYTYPE_ARRAY;copy.srcArray=array;
          copy.dstMemoryType=CU_MEMORYTYPE_DEVICE;copy.dstDevice=allocation;copy.dstPitch=pitch;
          copy.WidthInBytes=width*(rgb_input?4:1);copy.Height=rgb_input?height:height*3/2;cu(cuMemcpy2DAsync(&copy,stream));
          if (interop!=2 && interop!=11) {cu(cuGraphicsUnmapResources(1,&graphics_resource,stream));graphics_mapped=false;}
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
          if(interop==11)semaphore_bridge->copied(stream);
#endif
        }
        require(glGetError()==GL_NO_ERROR,"GL interop source update failed");
      } else if (opengl) { glClearTexSubImage(texture,0,0,row,0,width,64,1,rgb_input ? GL_RGBA : GL_RED,GL_UNSIGNED_BYTE,rgb_input ? rgba : &value); if (pipelined) glFlush(); else glFinish(); require(glGetError()==GL_NO_ERROR, "GL input update failed"); }
      else if (rgb_input) {
        // ABGR is packed A8B8G8R8: little-endian CUDA storage is R,G,B,A.
        unsigned pixel=unsigned(gray)|(unsigned(gray)<<8)|(unsigned(gray)<<16)|0xff000000u;
        cu(cuMemsetD2D32Async(allocation+row*pitch,pitch,pixel,width,64,stream));
        if(!pipelined)cu(cuStreamSynchronize(stream));
      } else if (pipelined) { cu(cuMemsetD2D8Async(allocation+row*pitch, pitch, value, width, 64, stream)); }
      else { cu(cuMemsetD2D8(allocation+row*pitch, pitch, value, width, 64)); cu(cuCtxSynchronize()); }
      auto ready=Clock::now();
      NV_ENC_MAP_INPUT_RESOURCE map {};map.version=NV_ENC_MAP_INPUT_RESOURCE_VER;map.registeredResource=paired && shared_copy ? copy_registered : registered;
      auto mapped_status=api.nvEncMapInputResource(encoder,&map);
      if(mapped_status!=NV_ENC_SUCCESS) {
        const char *detail=api.nvEncGetLastErrorString ? api.nvEncGetLastErrorString(encoder) : nullptr;
        throw std::runtime_error("NVENC input map status "+std::to_string(mapped_status)+": "+(detail ? detail : "no driver detail"));
      }
      mapped=map.mappedResource;
      if(output_in_vram) {
        NV_ENC_MAP_INPUT_RESOURCE output {};output.version=NV_ENC_MAP_INPUT_RESOURCE_VER;output.registeredResource=output_registered;
        check_nv(api.nvEncMapInputResource(encoder,&output),"NVENC map GPU output");output_mapped=output.mappedResource;
        bitstream=output_mapped;
      }
      NV_ENC_PIC_PARAMS picture {}; picture.version=NV_ENC_PIC_PARAMS_VER;
      picture.inputWidth=width;picture.inputHeight=height;picture.inputPitch=interop>=6 && interop<=10 && !shared_copy ? width : static_cast<unsigned>(pitch);
      picture.inputBuffer=mapped; picture.outputBitstream=bitstream; picture.bufferFmt=input_format;
      picture.pictureStruct=NV_ENC_PIC_STRUCT_FRAME; picture.inputTimeStamp=frame; picture.inputDuration=1; picture.frameIdx=frame;
      check_nv(api.nvEncEncodePicture(encoder,&picture),"NVENC encode picture");
      Clock::time_point done;
      if(output_in_vram) {
        // The documented synchronous GPU-output route uses unmap and default-stream reads.
        // It must not call LockBitstream or read an outstanding mapped output.
        check_nv(api.nvEncUnmapInputResource(encoder,mapped),"NVENC retire input");mapped=nullptr;
        check_nv(api.nvEncUnmapInputResource(encoder,output_mapped),"NVENC retire GPU output");output_mapped=nullptr;bitstream=nullptr;
        NV_ENC_ENCODE_OUT_PARAMS metadata {};
        cu(cuMemcpyDtoH(&metadata,output_allocation,sizeof(metadata)));
        require(metadata.bitstreamSizeInBytes>0 && metadata.bitstreamSizeInBytes<=output_capacity-sizeof(metadata),"Invalid GPU bitstream size");
        packet.resize(metadata.bitstreamSizeInBytes);
        cu(cuMemcpyDtoH(packet.data(),output_allocation+sizeof(metadata),packet.size()));
        bytes+=packet.size();done=Clock::now();
      } else {
        NV_ENC_LOCK_BITSTREAM lock {}; lock.version=NV_ENC_LOCK_BITSTREAM_VER; lock.outputBitstream=bitstream; lock.doNotWait=bitstream_wait=="poll";
        auto completion_deadline=Clock::now()+std::chrono::milliseconds(100);
        while(true){
          auto status=api.nvEncLockBitstream(encoder,&lock);
          if(status==NV_ENC_SUCCESS)break;
          if(bitstream_wait=="poll" && status==NV_ENC_ERR_LOCK_BUSY){
            ++busy_responses;require(Clock::now()<completion_deadline,"Bounded NVENC completion polling expired");
            if((busy_responses&255)==0)std::this_thread::yield();
            continue;
          }
          check_nv(status,"NVENC lock bitstream");
        }
        locked=true;done=Clock::now();
        require(lock.hwEncodeStatus==0,"NVENC reported an incomplete hardware result");
        require(lock.outputTimeStamp==frame && lock.bitstreamSizeInBytes>0, "Wrong timestamp or empty output"); bytes+=lock.bitstreamSizeInBytes;
        if (fixture) packet.assign(static_cast<unsigned char *>(lock.bitstreamBufferPtr),static_cast<unsigned char *>(lock.bitstreamBufferPtr)+lock.bitstreamSizeInBytes);
        nv(api.nvEncUnlockBitstream(encoder, bitstream)); locked=false;
        nv(api.nvEncUnmapInputResource(encoder, mapped)); mapped=nullptr;
      }
      if (interop==3) {
        nv(api.nvEncUnregisterResource(encoder, registered)); registered=nullptr;
        cu(cuGraphicsUnmapResources(1,&graphics_resource,stream)); graphics_mapped=false;
      }
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
      if(interop>=5 && interop<=7) shared->retire(stream);
#endif
      if (ready_event) {cu(cuEventDestroy(ready_event));ready_event=nullptr;}
      if (ready_sync!=EGL_NO_SYNC) {eglDestroySync(display,ready_sync);ready_sync=EGL_NO_SYNC;}
      auto retired=Clock::now();
      if (fixture) require(std::fwrite(packet.data(),1,packet.size(),fixture.get())==packet.size(),"Fixture write failed");
      if (frame>=20) {
        readiness.push_back(elapsed(begin,ready));encode.push_back(elapsed(ready,done));total.push_back(elapsed(begin,done));cycle.push_back(elapsed(begin,retired));
        if(paired) {(shared_copy ? copy_total : direct_total).push_back(elapsed(begin,done));(shared_copy ? copy_cycle : direct_cycle).push_back(elapsed(begin,retired));}
      }
    }
    auto stats=[](std::vector<double> values) { std::sort(values.begin(),values.end());char data[160];size_t n=values.size();require(n>0,"Missing timing samples");std::snprintf(data,sizeof(data),"{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}",values[n/2],values[n*95/100],values[n*99/100]);return std::string(data); };
    if(interop==7 || interop==10) {
      std::printf("{\"backend\":\"%s\",\"width\":%u,\"height\":%u,\"codec\":\"hevc\",\"round\":%d,\"pace_fps\":%u,\"samples_per_path\":50,\"bytes\":%llu,\"direct_output_us\":%s,\"copy_output_us\":%s,\"direct_cycle_us\":%s,\"copy_cycle_us\":%s}\n",backend(opengl,interop),width,height,round,pace_fps,bytes,stats(direct_total).c_str(),stats(copy_total).c_str(),stats(direct_cycle).c_str(),stats(copy_cycle).c_str());
      std::fflush(stdout);return;
    }
    std::printf("{\"bitstream_wait\":\"%s\",\"lock_busy_responses\":%llu,\"thread_cpu_us_per_frame_including_fixture_io\":%.3f,\"thread_cpu_frames\":120,\"backend\":\"%s\",\"width\":%u,\"height\":%u,\"video_clock_before_mhz\":%u,\"video_clock_after_mhz\":%u,\"graphics_clock_before_mhz\":%u,\"graphics_clock_after_mhz\":%u,\"output_memory\":\"%s\",\"encode_policy\":\"%s\",\"input_format\":\"%s\",\"matched_gray_pattern\":%s,\"codec\":\"%s\",\"round\":%d,\"requested_split\":%u,\"api_split_setting\":%u,\"stable_source\":%s,\"requested_priority\":\"%s\",\"granted_egl_priority\":%d,\"samples\":100,\"bytes\":%llu,\"source_ready_before_encode_clock\":%s,\"source_stage_us\":%s,\"encode_us\":%s,\"combined_us\":%s,\"cycle_us\":%s}\n",bitstream_wait.c_str(),busy_responses,(thread_cpu()-cpu_start)/120,backend(opengl,interop),width,height,video_clock_before,clocks().mhz(3),graphics_clock_before,clocks().mhz(0),output_in_vram?"gpu":"cpu",policy_name(),rgb_input ? "rgba-abgr" : "nv12",matched_gray_pattern ? "true" : "false",codec_name(av1),round,unsigned(split_policy),api_split_setting,stable_source ? "true" : "false",high ? "high" : "normal",granted_priority,bytes,pipelined && !stable_source ? "false" : "true",stats(readiness).c_str(),stats(encode).c_str(),stats(total).c_str(),stats(cycle).c_str());
    std::fflush(stdout);
  }
#ifdef SUNSHINE_MOTION_PROBE
  /** Full completed-packet span, including native input copy and optional pixel-flow/hint work. */
  void run_motion(int mode,int round,const std::filesystem::path &fixtures) {
    motion_hints_enabled=mode!=0;init(false,false,false);
    PixelFrames sources(width,height);
    std::unique_ptr<PixelFlow> flow;
    if(mode==3)flow=std::make_unique<PixelFlow>(cudacontext,stream,width,height);
    // Extra initialized rows cover the coded-height alignment without out-of-bounds hint storage.
    std::vector<NVENC_EXTERNAL_ME_HINT> hints(size_t((width+15)/16)*((height+63)/64*4));
    static_assert(sizeof(NVENC_EXTERNAL_ME_HINT)==4,"Unexpected external-hint ABI");
    const char *names[]={"internal","oracle","wrong","pixel-flow"};
    std::unique_ptr<FILE,FileCloser> fixture;
    if(!fixtures.empty()) {
      auto name=fixtures/("pixel-motion-"+std::to_string(width)+"x"+std::to_string(height)+"-"+names[mode]+"-"+std::to_string(round)+".hevc");
      fixture.reset(std::fopen(name.c_str(),"wb"));require(bool(fixture),"Motion fixture open failed");
      auto source_name=fixtures/("pixel-motion-"+std::to_string(width)+"x"+std::to_string(height)+"-source.nv12");
      std::unique_ptr<FILE,FileCloser> source(std::fopen(source_name.c_str(),"wb"));require(bool(source),"Motion source fixture open failed");
      for(unsigned index:{0u,3u}){auto pixels=sources.source(index);require(std::fwrite(pixels.data(),1,pixels.size(),source.get())==pixels.size(),"Motion source fixture write failed");}
    }
    std::vector<double> source_submit,hint_stage,encoding,cycle;
    std::vector<unsigned char> packet;uint64_t total_bytes=0,timed_bytes=0;
    auto cadence=Clock::now();
    for(unsigned frame=0;frame<120;++frame) {
      if(pace_fps)std::this_thread::sleep_until(cadence+std::chrono::nanoseconds(uint64_t(frame)*1000000000/pace_fps));
      auto begin=Clock::now();sources.copy(frame,allocation,pitch,stream);auto submitted=Clock::now();
      if(flow)flow->execute(frame,allocation,pitch);
      if(mode)pixel_hints(hints,width,height,frame,mode,flow.get());
      auto prepared=Clock::now();
      NV_ENC_MAP_INPUT_RESOURCE map {};map.version=NV_ENC_MAP_INPUT_RESOURCE_VER;map.registeredResource=registered;
      check_nv(api.nvEncMapInputResource(encoder,&map),"motion input map");mapped=map.mappedResource;
      NV_ENC_PIC_PARAMS picture {};picture.version=NV_ENC_PIC_PARAMS_VER;picture.inputWidth=width;picture.inputHeight=height;
      picture.inputPitch=static_cast<unsigned>(pitch);picture.inputBuffer=mapped;picture.outputBitstream=bitstream;picture.bufferFmt=input_format;
      picture.pictureStruct=NV_ENC_PIC_STRUCT_FRAME;picture.inputTimeStamp=frame;picture.inputDuration=1;picture.frameIdx=frame;
      if(mode) {picture.meHintCountsPerBlock[0].numCandsPerBlk16x16=1;picture.meExternalHints=hints.data();picture.meHintRefPicDist[0]=1;}
      check_nv(api.nvEncEncodePicture(encoder,&picture),"motion encode picture");
      NV_ENC_LOCK_BITSTREAM lock {};lock.version=NV_ENC_LOCK_BITSTREAM_VER;lock.outputBitstream=bitstream;
      check_nv(api.nvEncLockBitstream(encoder,&lock),"motion lock bitstream");locked=true;auto done=Clock::now();
      require(lock.outputTimeStamp==frame && lock.bitstreamSizeInBytes>0,"Motion output timestamp/length mismatch");
      total_bytes+=lock.bitstreamSizeInBytes;if(frame>=20)timed_bytes+=lock.bitstreamSizeInBytes;
      if(fixture)packet.assign(static_cast<unsigned char*>(lock.bitstreamBufferPtr),static_cast<unsigned char*>(lock.bitstreamBufferPtr)+lock.bitstreamSizeInBytes);
      check_nv(api.nvEncUnlockBitstream(encoder,bitstream),"motion unlock");locked=false;
      check_nv(api.nvEncUnmapInputResource(encoder,mapped),"motion unmap");mapped=nullptr;auto retired=Clock::now();
      if(fixture)require(std::fwrite(packet.data(),1,packet.size(),fixture.get())==packet.size(),"Motion stream write failed");
      if(flow)flow->validate(frame); // Independent known-motion diagnostic, excluded from measured span.
      if(frame>=20){source_submit.push_back(elapsed(begin,submitted));hint_stage.push_back(elapsed(submitted,prepared));encoding.push_back(elapsed(prepared,done));cycle.push_back(elapsed(begin,retired));}
    }
    auto stats=[](std::vector<double> v){std::sort(v.begin(),v.end());char s[160];auto n=v.size();require(n>0,"No motion samples");std::snprintf(s,sizeof(s),"{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}",v[n/2],v[n*95/100],v[n*99/100]);return std::string(s);};
    std::printf("{\"kind\":\"captured-pixel motion hints\",\"width\":%u,\"height\":%u,\"mode\":\"%s\",\"round\":%d,\"split\":%u,\"pace_fps\":%u,\"warmup\":20,\"samples\":100,\"total_bytes\":%llu,\"timed_bytes\":%llu,\"source_submit_us\":%s,\"motion_and_hints_us\":%s,\"encode_us\":%s,\"cycle_us\":%s,\"flow_checked\":%zu,\"flow_within_1px_fraction\":%.6f,\"flow_mean_axis_error_px\":%.6f}\n",width,height,names[mode],round,unsigned(split_policy),pace_fps,static_cast<unsigned long long>(total_bytes),static_cast<unsigned long long>(timed_bytes),stats(source_submit).c_str(),stats(hint_stage).c_str(),stats(encoding).c_str(),stats(cycle).c_str(),flow?flow->checked:0,flow&&flow->checked?double(flow->accurate)/flow->checked:0.,flow&&flow->checked?flow->absolute_error/flow->checked:0.);
    std::fflush(stdout);
  }
#endif
};
/** @brief Run reversed-order matched native input paths using an isolated driver handle. */
int main(int argc,char **argv) {
  void *library=dlopen("libnvidia-encode.so.1",RTLD_NOW|RTLD_LOCAL);
  if (!library) { std::fprintf(stderr,"NVENC driver unavailable\n"); return 1; }
  int result=0;
  try {
    std::filesystem::path fixtures;
    bool pipeline_matrix=false,interop_matrix=false,split_matrix=false,rgb_matrix=false,rgb_array_matrix=false,fourk_matrix=false,output_matrix=false,resolution_matrix=false,array_matrix=false,shared_matrix=false,shared_paired_matrix=false,native_array_matrix=false,native_paired_matrix=false,motion_matrix=false,semaphore_matrix=false;
    for (int i=1;i<argc;++i) {
      std::string arg=argv[i];
      if (arg=="--pipelined-probe") pipeline_matrix=true;
#ifdef SUNSHINE_MOTION_PROBE
      else if(arg=="--motion-probe")motion_matrix=true;
      else if(arg=="--motion-case" && i+1<argc) {
        std::string value=argv[++i];require(value=="internal"||value=="oracle"||value=="wrong"||value=="pixel-flow","Unknown motion case");
        motion_case=value=="internal"?0:value=="oracle"?1:value=="wrong"?2:3;
      }
      else if(arg=="--motion-size" && i+1<argc) {
        std::string value=argv[++i];require(value=="4k"||value=="1080p","Unknown motion geometry");Probe::width=value=="4k"?3840:1920;Probe::height=value=="4k"?2160:1080;
      }
#endif
      else if (arg=="--interop-probe") interop_matrix=true;
      else if (arg=="--split-probe") split_matrix=true;
      else if (arg=="--rgb-probe") rgb_matrix=true;
      else if (arg=="--rgb-array-probe") rgb_array_matrix=true;
      else if (arg=="--4k-policy-probe") fourk_matrix=true;
      else if (arg=="--4k-output-probe") output_matrix=true;
      else if (arg=="--no-filler") disable_filler_data=true;
      else if (arg=="--resolution-probe") resolution_matrix=true;
      else if (arg=="--4k-array-probe") array_matrix=true;
      else if (arg=="--4k-native-array-probe") native_array_matrix=true;
      else if (arg=="--4k-native-paired-probe") native_paired_matrix=true;
      else if (arg=="--pace-fps" && i+1<argc) {std::string rate=argv[++i];require(rate=="0" || rate=="120","Supported pacing controls are 0/120");pace_fps=rate=="120" ? 120 : 0;}
      else if(arg=="--green-sm" && i+1<argc) {std::string value=argv[++i];size_t parsed=0;auto count=std::stoul(value,&parsed);require(parsed==value.size() && count<=128,"Invalid green context SM count");green_sm_count=static_cast<unsigned>(count);}
      else if(arg=="--native-codec" && i+1<argc) {native_codec=argv[++i];require(native_codec=="hevc" || native_codec=="h264","Native codec must be HEVC or H.264");}
      else if(arg=="--h264-cavlc")h264_cavlc=true;
      else if(arg=="--bitstream-wait" && i+1<argc){bitstream_wait=argv[++i];require(bitstream_wait=="blocking"||bitstream_wait=="poll","Invalid completion wait policy");}
      else if(arg=="--context-wait" && i+1<argc) {context_wait=argv[++i];require(context_wait=="blocking" || context_wait=="spin" || context_wait=="yield" || context_wait=="auto","Invalid context wait policy");}
      else if (arg=="--split-mode" && i+1<argc) {
        std::string mode=argv[++i];require(mode=="auto" || mode=="disabled" || mode=="forced","Invalid split mode");
        split_policy=mode=="auto" ? NV_ENC_SPLIT_AUTO_MODE : mode=="disabled" ? NV_ENC_SPLIT_DISABLE_MODE : NV_ENC_SPLIT_TWO_FORCED_MODE;
      }
      else if (arg=="--io-stream" && i+1<argc) {
        std::string mode=argv[++i];require(mode=="explicit" || mode=="legacy","Invalid I/O stream control");explicit_io_stream=mode=="explicit";
      }
#ifdef SUNSHINE_VULKAN_SHARED_PROBE
      else if (arg=="--4k-semaphore-probe") semaphore_matrix=true;
      else if (arg=="--4k-shared-probe") shared_matrix=true;
      else if (arg=="--4k-shared-paired-probe") shared_paired_matrix=true;
      else if (arg=="--shared-shader" && i+1<argc) shared_shader=argv[++i];
#endif
      else if (arg=="--write-streams" && i+1<argc) { fixtures=argv[++i]; require(std::filesystem::is_directory(fixtures),"Fixture directory missing"); }
      else throw std::runtime_error("Usage: nvenc_gl_probe [--pipelined-probe|--interop-probe|--split-probe|--rgb-probe|--4k-policy-probe|--resolution-probe|--4k-array-probe|--4k-shared-probe|--4k-shared-paired-probe] [--shared-shader PATH] [--write-streams EXISTING_DIRECTORY]");
    }
    require(unsigned(pipeline_matrix)+unsigned(interop_matrix)+unsigned(split_matrix)+unsigned(rgb_matrix)+unsigned(rgb_array_matrix)+unsigned(fourk_matrix)+unsigned(output_matrix)+unsigned(resolution_matrix)+unsigned(array_matrix)+unsigned(shared_matrix)+unsigned(shared_paired_matrix)+unsigned(native_array_matrix)+unsigned(native_paired_matrix)+unsigned(motion_matrix)+unsigned(semaphore_matrix)<=1,"Select only one probe matrix");
    require(native_codec=="hevc" || native_array_matrix,"H.264 is restricted to the native array matrix");
    require(!h264_cavlc || native_codec=="h264","CAVLC requires H.264");
    auto create=reinterpret_cast<decltype(&NvEncodeAPICreateInstance)>(dlsym(library,"NvEncodeAPICreateInstance"));
    require(create,"NVENC create instance unavailable");
    NV_ENCODE_API_FUNCTION_LIST api {}; api.version=NV_ENCODE_API_FUNCTION_LIST_VER; nv(create(&api));
#ifdef SUNSHINE_MOTION_PROBE
    if(motion_matrix) {
      for(int round=0;round<2;++round)for(int order=0;order<4;++order){int mode=round?3-order:order;if(motion_case>=0&&mode!=motion_case)continue;Probe p(api);p.run_motion(mode,round,fixtures);}
    } else
#endif
    if(semaphore_matrix){
      Probe::width=3840;Probe::height=2160;stable_source=false;
      for(int round=0;round<2;++round)for(bool av1:{false,true})for(int step=0;step<2;++step){
        int mode=(round ? 1-step : step) ? 11 : 1;Probe probe(api);probe.run(false,av1,false,round,fixtures,true,mode);
      }
    } else if(native_paired_matrix){
      Probe::width=3840;Probe::height=2160;stable_source=true;
      for(int round=0;round<2;++round){Probe probe(api);probe.run(false,false,false,round,fixtures,true,10);}
    } else if(native_array_matrix){
      Probe::width=3840;Probe::height=2160;stable_source=true;
      const int modes[]={0,9,8};
      for(int round=0;round<2;++round)for(int step=0;step<3;++step){
        Probe probe(api);probe.run(false,false,false,round,fixtures,true,modes[round ? 2-step : step]);
      }
    } else if(shared_paired_matrix){
      Probe::width=3840;Probe::height=2160;
      for(int round=0;round<2;++round){Probe probe(api);probe.run(false,false,false,round,fixtures,true,7);}
    } else if(shared_matrix){
      Probe::width=3840;Probe::height=2160;
      for(int round=0;round<2;++round)for(int step=0;step<2;++step){
        int mode=(round ? 1-step : step) ? 6 : 5;
        Probe probe(api);probe.run(false,false,false,round,fixtures,true,mode);
      }
    } else if(array_matrix){
      Probe::width=3840;Probe::height=2160;
      for(int round=0;round<2;++round)for(int step=0;step<2;++step){
        int mode=(round ? 1-step : step) ? 3 : 4;
        Probe probe(api);probe.run(false,false,false,round,fixtures,true,mode);
      }
    } else if(resolution_matrix){
      stable_source=true;
      const unsigned widths[]={1920,2560,3840},heights[]={1080,1440,2160};
      for(int round=0;round<2;++round)for(unsigned step=0;step<3;++step)for(unsigned mode=0;mode<2;++mode){
        unsigned index=round ? 2-step : step;Probe::width=widths[index];Probe::height=heights[index];
        split_policy=(round ? 1-mode : mode) ? NV_ENC_SPLIT_TWO_FORCED_MODE : NV_ENC_SPLIT_AUTO_MODE;
        Probe probe(api);probe.run(false,false,false,round,fixtures,true);
      }
    } else if(rgb_array_matrix){
      Probe::width=3840;Probe::height=2160;stable_source=false;matched_gray_pattern=true;
      for(int round=0;round<2;++round)for(int step=0;step<4;++step) {
        int mode=round?3-step:step;rgb_input=mode!=0;
        Probe probe(api);probe.run(false,false,false,round,fixtures,true,mode<2?0:mode==2?3:4);
      }
    } else if(output_matrix){
      require(!explicit_io_stream && split_policy==NV_ENC_SPLIT_DISABLE_MODE,"Output comparison requires --io-stream legacy --split-mode disabled");
      Probe::width=3840;Probe::height=2160;stable_source=true;
      for(int round=0;round<2;++round)for(int input=0;input<2;++input)for(int step=0;step<2;++step) {
        output_in_vram=bool(round?1-step:step);int interop=(round?1-input:input)?8:0;
        Probe probe(api);probe.run(false,false,false,round,fixtures,true,interop);
      }
    } else if(fourk_matrix){
      Probe::width=3840;Probe::height=2160;stable_source=true;
      for(int round=0;round<2;++round)for(unsigned step=0;step<4;++step){
        encode_policy=round ? 3-step : step;Probe probe(api);probe.run(false,false,false,round,fixtures,true);
      }
    } else if(rgb_matrix){
      matched_gray_pattern=true;
      // HEVC first: color and AV1 validation are separate gates, not assumed.
      for(int round=0;round<2;++round)for(bool steady:{true,false})for(int step=0;step<4;++step){
        int mode=round ? 3-step : step;rgb_input=bool(mode&1);stable_source=steady;
        Probe probe(api);probe.run(mode>=2,false,false,round,fixtures,true);
      }
    } else if(split_matrix){
      const NV_ENC_SPLIT_ENCODE_MODE policies[]={NV_ENC_SPLIT_DISABLE_MODE,NV_ENC_SPLIT_AUTO_MODE,NV_ENC_SPLIT_TWO_FORCED_MODE};
      for(int round=0;round<2;++round)for(bool av1:{false,true})for(bool steady:{false,true})for(int step=0;step<3;++step){
        split_policy=policies[round?2-step:step];stable_source=steady;
        Probe probe(api);probe.run(false,av1,false,round,fixtures,true);
      }
    } else if (interop_matrix) {
      int modes=egl_event_bridge() ? 3 : 2;
      if(modes==2)std::fputs("SKIP egl-event: cuEventCreateFromEGLSync unavailable from installed driver\n",stderr);
      for(int round=0;round<2;++round)for(bool av1:{false,true})for(int step=0;step<modes;++step){
        int mode=round ? modes-1-step : step;
        Probe probe(api);probe.run(false,av1,false,round,fixtures,true,mode);
      }
    } else if (pipeline_matrix) {
      for (int round=0;round<2;++round) for (bool av1:{false,true}) for (int step=0;step<4;++step) {
        int mode=round ? 3-step : step;
        Probe probe(api); probe.run(mode>=2,av1,false,round,fixtures,mode%2!=0);
      }
    } else {
      for (int round=0;round<2;++round) for (bool av1:{false,true}) for (int step=0;step<3;++step) {
        int mode=round ? 2-step : step;
        Probe probe(api); probe.run(mode!=0,av1,mode==2,round,fixtures);
      }
    }
  } catch (const std::exception &error) { std::fprintf(stderr,"%s\n",error.what()); result=1; }
  dlclose(library); return result;
}
