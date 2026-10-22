// SPDX-License-Identifier: GPL-3.0-only
/** @file vulkan_video_trace.h
 * @brief Optional probe-only CPU-wall Vulkan call attribution, no GPU waits added.
 */
#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
namespace vk_trace {
/** @brief Bounded atomic counters; driver/API waits are not pure hardware durations. */
struct Counter {std::atomic<unsigned long long> n{0},ns{0},max{0};};
inline std::array<Counter,7> counters;
inline std::atomic<bool> enabled{false};
inline bool poll_completion=false; ///< Probe-only wait-policy experiment, selected before device use.
inline std::atomic<unsigned long long> poll_checks{0};
/** @brief Execute the exact original operation and aggregate only while selected. */
template<class F> VkResult call(unsigned stage,F&&fn){if(!enabled.load(std::memory_order_relaxed))return fn();auto a=std::chrono::steady_clock::now();auto r=fn();auto d=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-a).count();auto&c=counters[stage];c.n.fetch_add(1,std::memory_order_relaxed);c.ns.fetch_add(d,std::memory_order_relaxed);auto old=c.max.load(std::memory_order_relaxed);while(old<(unsigned long long)d&&!c.max.compare_exchange_weak(old,d,std::memory_order_relaxed)){}return r;}
/** @brief Diagnose host notification delay without claiming completion before a fence signals. */
inline VkResult completion_wait(VkDevice d,uint32_t n,const VkFence*f,VkBool32 all,uint64_t t){
 if(!poll_completion||!t)return vkWaitForFences(d,n,f,all,t);
 const auto start=std::chrono::steady_clock::now();
 for(;;){
  uint32_t ready=0;
  for(uint32_t i=0;i<n;++i){
   VkResult status=vkGetFenceStatus(d,f[i]);
   if(enabled.load(std::memory_order_relaxed))poll_checks.fetch_add(1,std::memory_order_relaxed);
   if(status==VK_SUCCESS)++ready;else if(status!=VK_NOT_READY)return status;
  }
  if(all ? ready==n : ready>0)return vkWaitForFences(d,n,f,all,0);
  if(t!=UINT64_MAX&&static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count())>=t)return VK_TIMEOUT;
  std::this_thread::sleep_for(std::chrono::microseconds(50));
 }
}
inline VkResult VKAPI_CALL wait_fences(VkDevice d,uint32_t n,const VkFence*f,VkBool32 all,uint64_t t){return call(0,[&]{return completion_wait(d,n,f,all,t);});}
inline VkResult VKAPI_CALL wait_semaphores(VkDevice d,const VkSemaphoreWaitInfo*i,uint64_t t){return call(1,[&]{return vkWaitSemaphores(d,i,t);});}
inline VkResult VKAPI_CALL query_results(VkDevice d,VkQueryPool p,uint32_t first,uint32_t n,size_t size,void*data,VkDeviceSize stride,VkQueryResultFlags flags){return call(2,[&]{return vkGetQueryPoolResults(d,p,first,n,size,data,stride,flags);});}
inline VkResult VKAPI_CALL submit(VkQueue q,uint32_t n,const VkSubmitInfo*i,VkFence f){return call(3,[&]{return vkQueueSubmit(q,n,i,f);});}
inline VkResult VKAPI_CALL submit2(VkQueue q,uint32_t n,const VkSubmitInfo2*i,VkFence f){return call(4,[&]{return vkQueueSubmit2(q,n,i,f);});}
inline VkResult VKAPI_CALL fence_status(VkDevice d,VkFence f){return call(5,[&]{return vkGetFenceStatus(d,f);});}
inline VkResult VKAPI_CALL semaphore_value(VkDevice d,VkSemaphore s,uint64_t*v){return call(6,[&]{return vkGetSemaphoreCounterValue(d,s,v);});}
/** @brief Preserve unsupported functions as null and intercept only known signatures. */
inline PFN_vkVoidFunction wrap(const char*n,PFN_vkVoidFunction original){if(!original)return nullptr;
#define WRAP(name,fn) if(!std::strcmp(n,name))return reinterpret_cast<PFN_vkVoidFunction>(fn)
 WRAP("vkWaitForFences",wait_fences);WRAP("vkWaitSemaphores",wait_semaphores);WRAP("vkWaitSemaphoresKHR",wait_semaphores);WRAP("vkGetQueryPoolResults",query_results);WRAP("vkQueueSubmit",submit);WRAP("vkQueueSubmit2",submit2);WRAP("vkQueueSubmit2KHR",submit2);WRAP("vkGetFenceStatus",fence_status);WRAP("vkGetSemaphoreCounterValue",semaphore_value);WRAP("vkGetSemaphoreCounterValueKHR",semaphore_value);
#undef WRAP
 return original;}
inline PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d,const char*n){return wrap(n,vkGetDeviceProcAddr(d,n));}
/** @brief FFmpeg supports a caller-provided instance lookup in AVVulkanDeviceContext. */
inline PFN_vkVoidFunction VKAPI_CALL instance_proc(VkInstance i,const char*n){auto original=vkGetInstanceProcAddr(i,n);if(original&&!std::strcmp(n,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(device_proc);return wrap(n,original);}
/** @brief Emit aggregates after measurement; nested or concurrent spans are not additive. */
inline void report(){const char*names[]={"wait_fences","wait_semaphores","query_results","queue_submit","queue_submit2","fence_status","semaphore_value"};for(unsigned i=0;i<counters.size();++i){auto&c=counters[i];auto n=c.n.load();if(n)std::printf("VK_API {\"stage\":\"%s\",\"calls\":%llu,\"mean_us\":%.3f,\"max_us\":%.3f}\n",names[i],n,c.ns.load()/1000.0/n,c.max.load()/1000.0);}if(poll_completion)std::printf("VK_POLL {\"status_checks\":%llu,\"sleep_us\":50}\n",poll_checks.load());}
}
