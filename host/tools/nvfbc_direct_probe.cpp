// SPDX-License-Identifier: GPL-3.0-only
/** @file nvfbc_direct_probe.cpp
 * @brief Explicit-PID NvFBC 1.9 capture-to-CUDA capability and fresh-frame probe.
 * Intended for an owned Vulkan test app on an isolated D-Bus; no portal or modeset.
 */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <cuda.h>
#include <NvFBC.h>
#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
/** @brief Reject failed driver operations. */
static void gpu(CUresult result){if(result!=CUDA_SUCCESS){const char*name=nullptr;cuGetErrorName(result,&name);throw std::runtime_error(name?name:"CUDA failure");}}
/** @brief Own a complete isolated capture session and driver handles. */
struct Capture {
 void *library=nullptr; NVFBC_API_FUNCTION_LIST api{};NVFBC_SESSION_HANDLE handle=0;bool handle_valid=false,session=false;CUcontext context=nullptr;bool gl=false;NVFBC_TOGL_SETUP_PARAMS glsetup{};
 ~Capture(){if(handle_valid){if(session){NVFBC_DESTROY_CAPTURE_SESSION_PARAMS p{};p.dwVersion=NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;api.nvFBCDestroyCaptureSession(handle,&p);}NVFBC_DESTROY_HANDLE_PARAMS p{};p.dwVersion=NVFBC_DESTROY_HANDLE_PARAMS_VER;api.nvFBCDestroyHandle(handle,&p);}if(context)cuCtxDestroy(context);if(library)dlclose(library);}
 /** @brief Include the vendor error while never converting a failure into a sample. */
 void check(NVFBCSTATUS status,const char*stage){if(status!=NVFBC_SUCCESS){const char*why=api.nvFBCGetLastErrorStr&&handle_valid?api.nvFBCGetLastErrorStr(handle):"no session error string";throw std::runtime_error(std::string(stage)+" status="+std::to_string(status)+" "+(why?why:""));}}
 /** @brief Require the documented direct backend and an explicit application PID. */
 void init(unsigned pid,bool use_gl=false){
  gl=use_gl;
  std::fputs("stage: load NvFBC\n",stderr);
  library=dlopen("libnvidia-fbc.so.1",RTLD_NOW|RTLD_LOCAL);if(!library)throw std::runtime_error("NvFBC library unavailable");
  auto create=reinterpret_cast<PNVFBCCREATEINSTANCE>(dlsym(library,"NvFBCCreateInstance"));if(!create)throw std::runtime_error("NvFBC entrypoint unavailable");
  api.dwVersion=NVFBC_VERSION;check(create(&api),"create_instance");
  NVFBC_CREATE_HANDLE_PARAMS p{};p.dwVersion=NVFBC_CREATE_HANDLE_PARAMS_VER;p.bUseEGL=NVFBC_TRUE;p.eBackend=NVFBC_BACKEND_DIRECT;
  // Consumer-GPU compatibility used by upstream Sunshine PR2471 (GPL-3.0).
  static const unsigned compatibility[]={0xAEF57AC5,0x401D1A39,0x1B856BBE,0x9ED0CEBA};
  p.privateData=compatibility;p.privateDataSize=sizeof(compatibility);
  std::fputs("stage: create_handle\n",stderr);
  check(api.nvFBCCreateHandle(&handle,&p),"create_handle");handle_valid=true;
  std::fputs("stage: get_status\n",stderr);
  NVFBC_GET_STATUS_PARAMS status{};status.dwVersion=NVFBC_GET_STATUS_PARAMS_VER;status.dwPid=pid;status.dwDbusTimeoutMs=1500;
  check(api.nvFBCGetStatus(handle,&status),"get_status");
  std::printf("{\"library_version\":%u,\"capture_possible\":%s,\"can_create\":%s,\"targets\":%u}\n",status.dwNvFBCVersion,status.bIsCapturePossible?"true":"false",status.bCanCreateNow?"true":"false",status.dwCaptureTargetCount);std::fflush(stdout);
  if(!gl){
  std::fputs("stage: create CUDA context\n",stderr);
  gpu(cuInit(0));CUdevice device;gpu(cuDeviceGet(&device,0));
#if CUDA_VERSION >= 13000
  CUctxCreateParams params{};gpu(cuCtxCreate(&context,&params,CU_CTX_SCHED_BLOCKING_SYNC,device));
#else
  gpu(cuCtxCreate(&context,CU_CTX_SCHED_BLOCKING_SYNC,device));
#endif

  NVFBC_BIND_CONTEXT_PARAMS bind{};bind.dwVersion=NVFBC_BIND_CONTEXT_PARAMS_VER;check(api.nvFBCBindContext(handle,&bind),"bind_context");
  }
  NVFBC_CREATE_CAPTURE_SESSION_PARAMS capture{};capture.dwVersion=NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;capture.eCaptureType=gl?NVFBC_CAPTURE_TO_GL:NVFBC_CAPTURE_SHARED_CUDA;
  capture.bPushModel=NVFBC_TRUE;capture.bAllowDirectCapture=NVFBC_TRUE;capture.bWithCursor=NVFBC_FALSE;capture.dwPid=pid;capture.dwDbusTimeoutMs=1500;capture.dwCaptureTarget=0;
  std::fputs("stage: create_capture\n",stderr);
  check(api.nvFBCCreateCaptureSession(handle,&capture),"create_capture");session=true;
  if(gl){glsetup.dwVersion=NVFBC_TOGL_SETUP_PARAMS_VER;glsetup.eBufferFormat=NVFBC_BUFFER_FORMAT_BGRA;check(api.nvFBCToGLSetUp(handle,&glsetup),"setup_gl");}
  else {
  NVFBC_TOCUDA_SETUP_PARAMS setup{};setup.dwVersion=NVFBC_TOCUDA_SETUP_PARAMS_VER;setup.eBufferFormat=NVFBC_BUFFER_FORMAT_BGRA;
  std::fputs("stage: setup_cuda\n",stderr);
  check(api.nvFBCToCudaSetUp(handle,&setup),"setup_cuda");
  }
  std::fputs("stage: ready\n",stderr);
 }
 /** @brief Record fresh-frame counts and raw timestamps; their clock origin is not assumed. */
 void run(){
  std::vector<double> calls;unsigned duplicates=0,last=~0u,direct=0,postprocessed=0;NVFBC_FRAME_GRAB_INFO info{};CUdeviceptr pointer=0;uint64_t timestamp=0;
  for(unsigned attempt=0;attempt<240&&calls.size()<100;++attempt){
   NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab{};grab.dwVersion=NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER;grab.dwFlags=NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT_IF_NEW_FRAME_READY;grab.dwTimeoutMs=100;grab.pCUDADeviceBuffer=&pointer;grab.pFrameGrabInfo=&info;
   auto a=std::chrono::steady_clock::now();unsigned texture_index=0;
   if(gl){NVFBC_TOGL_GRAB_FRAME_PARAMS g{};g.dwVersion=NVFBC_TOGL_GRAB_FRAME_PARAMS_VER;g.dwFlags=NVFBC_TOGL_GRAB_FLAGS_NOWAIT_IF_NEW_FRAME_READY;g.dwTimeoutMs=100;g.pFrameGrabInfo=&info;check(api.nvFBCToGLGrabFrame(handle,&g),"grab_gl");texture_index=g.dwTextureIndex;}
   else check(api.nvFBCToCudaGrabFrame(handle,&grab),"grab");
   auto b=std::chrono::steady_clock::now();
   if((!gl&&!pointer)||!info.dwWidth||!info.dwHeight)throw std::runtime_error("Empty capture");
   unsigned char consumer_sample[16]{};
   if(gl){if(texture_index>=NVFBC_TOGL_TEXTURES_MAX)throw std::runtime_error("Invalid texture index");glGetTextureSubImage(glsetup.dwTextures[texture_index],0,0,0,0,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,sizeof(consumer_sample),consumer_sample);if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("GL consumer read failed");}
   else gpu(cuMemcpyDtoH(consumer_sample,pointer,sizeof(consumer_sample)));
   if(!info.bIsNewFrame||info.dwCurrentFrame==last){++duplicates;continue;}
   last=info.dwCurrentFrame;timestamp=info.ulTimestampUs;direct+=bool(info.bDirectCapture);postprocessed+=bool(info.bRequiredPostProcessing);
   calls.push_back(std::chrono::duration<double,std::micro>(b-a).count());
  }
  if(calls.size()!=100)throw std::runtime_error("Could not obtain 100 unique fresh frames");
  // A tiny read validates accessibility after capture, outside the capture-call span.
  unsigned char sample[16]{};if(!gl)gpu(cuMemcpyDtoH(sample,pointer+((info.dwHeight/2)*info.dwWidth+info.dwWidth/2)*4,sizeof(sample)));
  std::sort(calls.begin(),calls.end());
  std::printf("{\"width\":%u,\"height\":%u,\"unique_fresh_frames\":100,\"duplicate_returns\":%u,\"direct_frames\":%u,\"postprocessed_frames\":%u,\"raw_timestamp_us\":%llu,\"timestamp_clock_verified\":false,\"grab_call_us\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f},\"boundary\":\"capture API wait included; not host or input-to-photon latency\"}\n",info.dwWidth,info.dwHeight,duplicates,direct,postprocessed,(unsigned long long)timestamp,calls[50],calls[95],calls[99]);
 }
};
/** @brief Capture only an explicitly supplied live Vulkan PID. */
int main(int argc,char**argv){try{if(argc<2||argc>3)throw std::runtime_error("Usage: nvfbc_direct_probe OWNED_VULKAN_PID [--gl]");char*end=nullptr;auto pid=std::strtoul(argv[1],&end,10);if(!pid||!end||*end)throw std::runtime_error("Invalid PID");Capture capture;capture.init(pid,argc==3&&std::string(argv[2])=="--gl");capture.run();return 0;}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
