// SPDX-License-Identifier: GPL-3.0-only
/** @file vulkan_video_latency.cpp
 * @brief Ready-GPU static NV12 to completed Vulkan Video packets, no capture or upload in the loop.
 */
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdlib>
#include "vulkan_video_trace.h"
/** @brief Convert every failed API operation into an explicit failure. */
static void check(int rc){if(rc<0){char error[AV_ERROR_MAX_STRING_SIZE];av_strerror(rc,error,sizeof(error));throw std::runtime_error(error);}}
/** @brief Own partial device/codec/frame initialization and optional output. */
struct Probe {
 AVBufferRef *device=nullptr,*frames=nullptr;AVCodecContext*context=nullptr;AVFrame*cpu=nullptr,*gpu=nullptr;AVPacket*packet=nullptr;FILE*stream=nullptr;bool tracing=false;
 ~Probe(){if(stream)std::fclose(stream);av_packet_free(&packet);av_frame_free(&gpu);av_frame_free(&cpu);avcodec_free_context(&context);av_buffer_unref(&frames);av_buffer_unref(&device);}
 /** @brief Upload one static pattern; input storage remains unchanged throughout encode. */
 void init(const char*codec,const char*output){
  check(av_hwdevice_ctx_create(&device,AV_HWDEVICE_TYPE_VULKAN,"0",nullptr,0));
  auto vk=reinterpret_cast<AVVulkanDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx);VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(vk->phys_dev,&props);
  if(!std::strstr(props.deviceName,"NVIDIA"))throw std::runtime_error("Probe requires NVIDIA Vulkan device0");
  uint32_t family_count=0;vkGetPhysicalDeviceQueueFamilyProperties(vk->phys_dev,&family_count,nullptr);std::vector<VkQueueFamilyProperties> families(family_count);vkGetPhysicalDeviceQueueFamilyProperties(vk->phys_dev,&family_count,families.data());
  for(uint32_t i=0;i<family_count;++i)if(families[i].queueFlags&VK_QUEUE_VIDEO_ENCODE_BIT_KHR)std::printf("VK_QUEUE {\"family\":%u,\"timestamp_valid_bits\":%u,\"timestamp_period_ns\":%.3f}\n",i,families[i].timestampValidBits,props.limits.timestampPeriod);
  const char*trace=std::getenv("SUNSHINE_VK_TRACE");const char*poll=std::getenv("SUNSHINE_VK_POLL_FENCES");vk_trace::poll_completion=poll&&std::strcmp(poll,"1")==0;tracing=(trace&&std::strcmp(trace,"1")==0)||vk_trace::poll_completion;if(tracing)vk->get_proc_addr=vk_trace::instance_proc;
  frames=av_hwframe_ctx_alloc(device);if(!frames)throw std::runtime_error("Frame context allocation failed");
  auto fc=reinterpret_cast<AVHWFramesContext*>(frames->data);fc->format=AV_PIX_FMT_VULKAN;fc->sw_format=AV_PIX_FMT_NV12;fc->width=1920;fc->height=1080;fc->initial_pool_size=4;check(av_hwframe_ctx_init(frames));
  cpu=av_frame_alloc();gpu=av_frame_alloc();packet=av_packet_alloc();if(!cpu||!gpu||!packet)throw std::runtime_error("Frame allocation failed");
  cpu->format=AV_PIX_FMT_NV12;cpu->width=1920;cpu->height=1080;check(av_frame_get_buffer(cpu,0));
  for(int y=0;y<1080;++y)for(int x=0;x<1920;++x)cpu->data[0][y*cpu->linesize[0]+x]=16+((x/8+y/8)%220);
  for(int y=0;y<540;++y)std::memset(cpu->data[1]+y*cpu->linesize[1],128,1920);
  check(av_hwframe_get_buffer(frames,gpu,0));check(av_hwframe_transfer_data(gpu,cpu,0));
  auto encoder=avcodec_find_encoder_by_name(codec);if(!encoder)throw std::runtime_error("Encoder unavailable");context=avcodec_alloc_context3(encoder);if(!context)throw std::runtime_error("Codec context allocation failed");
  context->width=1920;context->height=1080;context->pix_fmt=AV_PIX_FMT_VULKAN;context->time_base={1,120};context->framerate={120,1};context->bit_rate=23000000;context->gop_size=32767;context->max_b_frames=0;context->flags|=AV_CODEC_FLAG_LOW_DELAY;
  context->hw_frames_ctx=av_buffer_ref(frames);context->hw_device_ctx=av_buffer_ref(device);
  for(auto item:{std::pair{"tune","ull"},std::pair{"rc_mode","cbr"},std::pair{"usage","stream"},std::pair{"content","rendered"}})check(av_opt_set(context->priv_data,item.first,item.second,0));
  check(av_opt_set_int(context->priv_data,"async_depth",1,0));check(av_opt_set_int(context->priv_data,"quality",0,0));check(av_opt_set_int(context->priv_data,"units",0,0));check(avcodec_open2(context,encoder,nullptr));
  if(output){stream=std::fopen(output,"wb");if(!stream)throw std::runtime_error("Output open failed");}
 }
 /** @brief Require one completed matching packet per frame; EAGAIN is not completion. */
 void run(const char*codec){
  std::vector<double> service;unsigned long long bytes=0;
  for(int i=0;i<120;++i){vk_trace::enabled.store(tracing&&i>=20);gpu->pts=i;auto start=std::chrono::steady_clock::now();check(avcodec_send_frame(context,gpu));check(avcodec_receive_packet(context,packet));auto stop=std::chrono::steady_clock::now();
   if(packet->pts!=i||packet->size<=0)throw std::runtime_error("Mismatched or empty packet");bytes+=packet->size;
   if(stream&&std::fwrite(packet->data,1,packet->size,stream)!=size_t(packet->size))throw std::runtime_error("Output write failed");
   if(i>=20)service.push_back(std::chrono::duration<double,std::micro>(stop-start).count());av_packet_unref(packet);
  }
  vk_trace::enabled.store(false);if(tracing)vk_trace::report();
  check(avcodec_send_frame(context,nullptr));if(avcodec_receive_packet(context,packet)!=AVERROR_EOF)throw std::runtime_error("Unexpected buffered output at drain");
  std::sort(service.begin(),service.end());std::printf("{\"codec\":\"%s\",\"width\":1920,\"height\":1080,\"samples\":100,\"warmup\":20,\"bytes\":%llu,\"source\":\"static ready GPU NV12\",\"service_us\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f},\"boundary\":\"encode only; no capture, conversion, frame pacing or TV\"}\n",codec,bytes,service[50],service[95],service[99]);
 }
};
/** @brief Explicit backend and optional temporary raw output path. */
int main(int argc,char**argv){try{if(argc>3)throw std::runtime_error("Usage: vulkan_video_latency [hevc_vulkan|av1_vulkan] [temporary_output]");const char*codec=argc>1?argv[1]:"hevc_vulkan";Probe p;p.init(codec,argc>2?argv[2]:nullptr);p.run(codec);return 0;}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
