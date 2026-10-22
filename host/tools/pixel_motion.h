// SPDX-License-Identifier: GPL-3.0-only
/** Owned captured-image analogues and hardware optical flow; never accesses a game. */
#pragma once
#include <nvOpticalFlowCuda.h>
#include <array>
#include <cmath>
#include <limits>

static int motion_case = -1; ///< -1=matrix, 0=baseline, 1=oracle, 2=wrong, 3=pixel flow.
static bool motion_hints_enabled = false;

/** Four pre-uploaded, deterministically translated NV12 surfaces. */
struct PixelFrames {
  unsigned width, height;
  std::array<CUdeviceptr,4> buffers {};
  std::array<size_t,4> pitches {};
  PixelFrames(unsigned w,unsigned h):width(w),height(h) {
    try {
      for(unsigned i=0;i<4;++i) {
        auto input=source(i);
        cu(cuMemAllocPitch(&buffers[i],&pitches[i],width,height*3/2,16));
        CUDA_MEMCPY2D copy {};copy.srcMemoryType=CU_MEMORYTYPE_HOST;copy.srcHost=input.data();copy.srcPitch=width;
        copy.dstMemoryType=CU_MEMORYTYPE_DEVICE;copy.dstDevice=buffers[i];copy.dstPitch=pitches[i];copy.WidthInBytes=width;copy.Height=height*3/2;
        cu(cuMemcpy2D(&copy));
        std::vector<unsigned char> actual(input.size());
        copy.srcMemoryType=CU_MEMORYTYPE_DEVICE;copy.srcDevice=buffers[i];copy.srcPitch=pitches[i];copy.dstMemoryType=CU_MEMORYTYPE_HOST;
        copy.dstHost=actual.data();copy.dstPitch=width;cu(cuMemcpy2D(&copy));
        require(actual==input,"Translated source readback mismatch");
      }
    } catch (...) {clear();throw;}
  }
  void clear(){for(auto &b:buffers)if(b){cuMemFree(b);b=0;}}
  ~PixelFrames(){clear();}
  PixelFrames(const PixelFrames&)=delete;
  static unsigned hash(unsigned x,unsigned y) {
    unsigned v=x*0x45d9f3bu^y*0x119de1f3u;v^=v>>16;v*=0x45d9f3bu;v^=v>>16;return v&255;
  }
  /** Spatially textured gradients, thin edges and chroma; translation changes at wrap. */
  std::vector<unsigned char> source(unsigned index) const {
    std::vector<unsigned char> out(size_t(width)*height*3/2);
    unsigned dx=(index%4)*4,dy=(index%4)*2;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
      unsigned sx=(x+width-dx)%width,sy=(y+height-dy)%height;
      unsigned xx=sx/32,yy=sy/32,fx=sx%32,fy=sy%32;
      unsigned top=hash(xx,yy)*(32-fx)+hash(xx+1,yy)*fx;
      unsigned bottom=hash(xx,yy+1)*(32-fx)+hash(xx+1,yy+1)*fx;
      unsigned value=(top*(32-fy)+bottom*fy+512)/1024;
      if(sx%83<2 || sy%61<2)value=230;
      out[size_t(y)*width+x]=static_cast<unsigned char>(16+(219*value+127)/255);
    }
    for(unsigned y=0;y<height/2;++y)for(unsigned x=0;x<width/2;++x) {
      unsigned sx=(x+width/2-dx/2)%(width/2),sy=(y+height/2-dy/2)%(height/2);
      out[size_t(width)*height+size_t(y)*width+x*2]=static_cast<unsigned char>(32+(sx/24+sy/17)%193);
      out[size_t(width)*height+size_t(y)*width+x*2+1]=static_cast<unsigned char>(32+(sx/33+sy/23)%193);
    }
    return out;
  }
  void copy(unsigned frame,CUdeviceptr dst,size_t pitch,CUstream stream)const {
    unsigned i=frame%4;CUDA_MEMCPY2D c {};c.srcMemoryType=CU_MEMORYTYPE_DEVICE;c.srcDevice=buffers[i];c.srcPitch=pitches[i];
    c.dstMemoryType=CU_MEMORYTYPE_DEVICE;c.dstDevice=dst;c.dstPitch=pitch;c.WidthInBytes=width;c.Height=height*3/2;
    cu(cuMemcpy2DAsync(&c,stream));
  }
  static std::pair<int,int> expected(unsigned frame) {
    if(!frame)return {0,0};
    int delta=int((frame-1)%4)-int(frame%4);
    return {delta*4,delta*2}; // Current pixel -> corresponding previous-frame position.
  }
};

/** Dedicated optical-flow engine via NVIDIA's pinned public 2.0 API. */
struct PixelFlow {
  void *library=nullptr;
  NV_OF_CUDA_API_FUNCTION_LIST api {};
  NvOFHandle handle=nullptr;
  std::array<NvOFGPUBufferHandle,4> inputs {};
  NvOFGPUBufferHandle output=nullptr;
  std::array<CUdeviceptr,4> pointers {};
  std::array<size_t,4> pitches {};
  CUdeviceptr output_pointer=0;
  size_t output_pitch=0;
  unsigned width,height,fw,fh;
  CUstream stream;
  NV_OF_FLOW_VECTOR *host=nullptr;
  double absolute_error=0;
  size_t accurate=0,checked=0;
  explicit PixelFlow(CUcontext context,CUstream s,unsigned w,unsigned h):width(w),height(h),fw((w+3)/4),fh((h+3)/4),stream(s) {
    try {
      library=dlopen("libnvidia-opticalflow.so.1",RTLD_NOW|RTLD_LOCAL);require(library,"Optical-flow driver unavailable");
      auto create=reinterpret_cast<decltype(&NvOFAPICreateInstanceCuda)>(dlsym(library,"NvOFAPICreateInstanceCuda"));
      require(create,"Optical-flow API entrypoint unavailable");of(create(NV_OF_API_VERSION,&api),"flow API");
      of(api.nvCreateOpticalFlowCuda(context,&handle),"flow context");
      uint32_t count=0;of(api.nvOFGetCaps(handle,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,nullptr,&count),"flow grid count");
      require(count>0 && count<32,"Invalid optical-flow grid capability count");std::vector<uint32_t> grids(count);
      of(api.nvOFGetCaps(handle,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,grids.data(),&count),"flow grids");
      require(std::find(grids.begin(),grids.end(),4)!=grids.end(),"Optical flow grid4 unsupported");
      NV_OF_INIT_PARAMS init {};init.width=width;init.height=height;init.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
      init.mode=NV_OF_MODE_OPTICALFLOW;init.perfLevel=NV_OF_PERF_LEVEL_FAST;
      of(api.nvOFInit(handle,&init),"flow initialize");
      of(api.nvOFSetIOCudaStreams(handle,stream,stream),"flow streams");
      for(unsigned i=0;i<4;++i) {
        NV_OF_BUFFER_DESCRIPTOR b {};b.width=width;b.height=height;b.bufferUsage=NV_OF_BUFFER_USAGE_INPUT;b.bufferFormat=NV_OF_BUFFER_FORMAT_GRAYSCALE8;
        of(api.nvOFCreateGPUBufferCuda(handle,&b,NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,&inputs[i]),"flow input buffer");
        pointers[i]=api.nvOFGPUBufferGetCUdeviceptr(inputs[i]);require(pointers[i],"Null flow input allocation");
        NV_OF_CUDA_BUFFER_STRIDE_INFO stride {};of(api.nvOFGPUBufferGetStrideInfo(inputs[i],&stride),"flow input stride");
        require(stride.numPlanes==1 && stride.strideInfo[0].strideXInBytes>=width,"Invalid flow input layout");pitches[i]=stride.strideInfo[0].strideXInBytes;
      }
      NV_OF_BUFFER_DESCRIPTOR b {};b.width=fw;b.height=fh;b.bufferUsage=NV_OF_BUFFER_USAGE_OUTPUT;b.bufferFormat=NV_OF_BUFFER_FORMAT_SHORT2;
      of(api.nvOFCreateGPUBufferCuda(handle,&b,NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,&output),"flow output buffer");
      output_pointer=api.nvOFGPUBufferGetCUdeviceptr(output);require(output_pointer,"Null flow output allocation");
      NV_OF_CUDA_BUFFER_STRIDE_INFO stride {};of(api.nvOFGPUBufferGetStrideInfo(output,&stride),"flow output stride");output_pitch=stride.strideInfo[0].strideXInBytes;
      require(output_pitch>=fw*sizeof(NV_OF_FLOW_VECTOR),"Invalid flow output layout");
      cu(cuMemAllocHost(reinterpret_cast<void**>(&host),size_t(fw)*fh*sizeof(NV_OF_FLOW_VECTOR)));
    } catch (...) {clear();throw;}
  }
  void of(NV_OF_STATUS status,const char *stage) const {
    if(status==NV_OF_SUCCESS)return;
    throw std::runtime_error(std::string(stage)+" optical-flow status "+std::to_string(status));
  }
  void clear() {
    if(handle && api.nvOFDestroyGPUBufferCuda){for(auto b:inputs)if(b)api.nvOFDestroyGPUBufferCuda(b);if(output)api.nvOFDestroyGPUBufferCuda(output);}
    if(handle && api.nvOFDestroy)api.nvOFDestroy(handle);handle=nullptr;
    if(host)cuMemFreeHost(host);host=nullptr;
    if(library)dlclose(library);library=nullptr;
  }
  ~PixelFlow(){clear();}
  PixelFlow(const PixelFlow&)=delete;
  void execute(unsigned frame,CUdeviceptr source,size_t source_pitch) {
    unsigned i=frame%4;CUDA_MEMCPY2D c {};c.srcMemoryType=CU_MEMORYTYPE_DEVICE;c.srcDevice=source;c.srcPitch=source_pitch;
    c.dstMemoryType=CU_MEMORYTYPE_DEVICE;c.dstDevice=pointers[i];c.dstPitch=pitches[i];c.WidthInBytes=width;c.Height=height;
    cu(cuMemcpy2DAsync(&c,stream));
    if(!frame){cu(cuStreamSynchronize(stream));return;}
    NV_OF_EXECUTE_INPUT_PARAMS in {};in.inputFrame=inputs[i];in.referenceFrame=inputs[(frame-1)%4];
    NV_OF_EXECUTE_OUTPUT_PARAMS out {};out.outputBuffer=output;
    of(api.nvOFExecute(handle,&in,&out),"flow execute");
    c={};c.srcMemoryType=CU_MEMORYTYPE_DEVICE;c.srcDevice=output_pointer;c.srcPitch=output_pitch;
    c.dstMemoryType=CU_MEMORYTYPE_HOST;c.dstHost=host;c.dstPitch=fw*sizeof(NV_OF_FLOW_VECTOR);
    c.WidthInBytes=c.dstPitch;c.Height=fh;cu(cuMemcpy2DAsync(&c,stream));cu(cuStreamSynchronize(stream));
  }
  std::pair<int,int> vector(unsigned x,unsigned y)const {
    const auto &v=host[size_t(std::min(fh-1,y/4))*fw+std::min(fw-1,x/4)];
    return {int(std::lround(v.flowx/32.)),int(std::lround(v.flowy/32.))};
  }
  void validate(unsigned frame) {
    if(frame<20)return;
    auto expected=PixelFrames::expected(frame);
    for(unsigned y=32;y+32<height;y+=32)for(unsigned x=32;x+32<width;x+=32) {
      const auto &v=host[size_t(y/4)*fw+x/4];double dx=std::abs(v.flowx/32.-expected.first),dy=std::abs(v.flowy/32.-expected.second);
      absolute_error+=(dx+dy)/2;accurate+=(dx<=1 && dy<=1);++checked;
    }
  }
};

/** Build one integer-pixel L0 hint per16x16 block, including padded edge storage. */
static void pixel_hints(std::vector<NVENC_EXTERNAL_ME_HINT>&hints,unsigned w,unsigned h,unsigned frame,int mode,const PixelFlow *flow) {
  unsigned mw=(w+15)/16,mh=(h+63)/64*4;
  require(hints.size()>=size_t(mw)*mh,"Hint allocation too small");
  auto expected=PixelFrames::expected(frame);
  for(unsigned by=0;by<mh;++by)for(unsigned bx=0;bx<mw;++bx) {
    auto motion=mode==3 && frame ? flow->vector(std::min(w-1,bx*16+8),std::min(h-1,by*16+8)) : expected;
    if(mode==2){motion.first=-motion.first;motion.second=-motion.second;}
    auto &v=hints[size_t(by)*mw+bx];v={};
    v.mvx=std::clamp(motion.first,-2048,2047);v.mvy=std::clamp(motion.second,-512,511);
    v.refidx=0;v.dir=0;v.partType=0;v.lastofPart=-1;v.lastOfMB=-1;
  }
}
