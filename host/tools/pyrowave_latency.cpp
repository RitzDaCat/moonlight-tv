// SPDX-License-Identifier: GPL-3.0-only
/** Completed packet/readback benchmark for a pinned upstream intra-only GPU codec. */
#include <vulkan/vulkan.h>
#include <pyrowave.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Clock=std::chrono::steady_clock;
static void require(bool yes,const char *label){if(!yes)throw std::runtime_error(label);}
static void vk(VkResult r){if(r!=VK_SUCCESS)throw std::runtime_error("Vulkan "+std::to_string(r));}
static void pyro(pyrowave_result r){if(r!=PYROWAVE_SUCCESS)throw std::runtime_error("PyroWave "+std::to_string(r));}
static double us(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::micro>(b-a).count();}
static std::string quantiles(std::vector<double> a){std::sort(a.begin(),a.end());require(!a.empty(),"Missing samples");char s[160];std::snprintf(s,sizeof(s),"{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}",a[a.size()/2],a[a.size()*95/100],a[a.size()*99/100]);return s;}
// Read-only driver telemetry; no clock locking or power policy changes.
struct Telemetry {
  void *lib=nullptr,*device=nullptr;bool initialized=false;int (*clock)(void *,int,unsigned *)=nullptr;int (*shutdown)()=nullptr;
  Telemetry(){lib=dlopen("libnvidia-ml.so.1",RTLD_NOW|RTLD_LOCAL);if(!lib)return;auto init=reinterpret_cast<int(*)()>(dlsym(lib,"nvmlInit_v2"));auto handle=reinterpret_cast<int(*)(unsigned,void **)>(dlsym(lib,"nvmlDeviceGetHandleByIndex_v2"));clock=reinterpret_cast<decltype(clock)>(dlsym(lib,"nvmlDeviceGetClockInfo"));shutdown=reinterpret_cast<decltype(shutdown)>(dlsym(lib,"nvmlShutdown"));if(init&&handle&&clock&&shutdown&&init()==0){initialized=true;if(handle(0,&device))device=nullptr;}}
  ~Telemetry(){if(initialized)shutdown();if(lib)dlclose(lib);}
  unsigned mhz(){unsigned value=0;if(device&&clock(device,0,&value))return 0;return value;}
};

struct Device {
  pyrowave_device pyrodev=nullptr;
  VkInstance instance=VK_NULL_HANDLE;
  bool owns_device=false;
  double occupancy=-1;
  bool timestamps=false;
  VkDevice device=VK_NULL_HANDLE;
  VkPhysicalDevice physical=VK_NULL_HANDLE;
  VkQueue queue=VK_NULL_HANDLE;
  VkCommandPool pool=VK_NULL_HANDLE;
  VkQueue compute_queue=VK_NULL_HANDLE;
  VkCommandPool compute_pool=VK_NULL_HANDLE;
  VkSemaphore timeline=VK_NULL_HANDLE;
  VkQueryPool query=VK_NULL_HANDLE;
  PFN_vkGetCalibratedTimestampsEXT calibrated=nullptr;
  double timestamp_period=0,clock_offset_ns=0,last_gpu_us=0,last_queue_us=0,last_notify_us=0;
  uint64_t calibration_deviation_ns=0;
  Clock::time_point submitted,woken,calibrated_at;
  std::vector<uint32_t> families;
  uint32_t graphics=0;
  uint32_t compute_family=0;
  uint64_t signal=0;
  VkApplicationInfo application {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  VkInstanceCreateInfo instance_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan11Features features11 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features features12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features features13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceComputeOccupancyPriorityFeaturesNV occupancy_feature {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_OCCUPANCY_PRIORITY_FEATURES_NV};
  VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  std::vector<VkDeviceQueueCreateInfo> queue_info;
  std::vector<const char *> extensions;
  float queue_priority=1;
  PFN_vkCmdSetComputeOccupancyPriorityNV set_occupancy=nullptr;
  // Explicit app-owned device makes feature/priority control measurable without privileges.
  void create_custom(){
    application.apiVersion=VK_API_VERSION_1_3;instance_info.pApplicationInfo=&application;
    vk(vkCreateInstance(&instance_info,nullptr,&instance));owns_device=true;
    uint32_t n=0;vk(vkEnumeratePhysicalDevices(instance,&n,nullptr));std::vector<VkPhysicalDevice> candidates(n);vk(vkEnumeratePhysicalDevices(instance,&n,candidates.data()));
    for(auto candidate:candidates){VkPhysicalDeviceProperties p {};vkGetPhysicalDeviceProperties(candidate,&p);if(p.vendorID==0x10de){physical=candidate;break;}}
    require(physical,"NVIDIA Vulkan device unavailable");
    uint32_t count=0;vk(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr));std::vector<VkExtensionProperties> available(count);vk(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,available.data()));
    auto has=[&](const char *name){return std::any_of(available.begin(),available.end(),[&](const auto &e){return !std::strcmp(e.extensionName,name);});};
    require(has(VK_NV_COMPUTE_OCCUPANCY_PRIORITY_EXTENSION_NAME),"Occupancy extension not advertised");extensions.push_back(VK_NV_COMPUTE_OCCUPANCY_PRIORITY_EXTENSION_NAME);
    if(timestamps){require(has(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME),"Calibrated timestamp extension unavailable");extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);}
    if(has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    features.pNext=&features11;features11.pNext=&features12;features12.pNext=&features13;features13.pNext=&occupancy_feature;
    vkGetPhysicalDeviceFeatures2(physical,&features);require(occupancy_feature.computeOccupancyPriority,"Occupancy feature unsupported");
    require(features.features.shaderInt16 && features12.shaderFloat16 && features12.storageBuffer8BitAccess && features12.timelineSemaphore && features13.subgroupSizeControl,"Required codec features unavailable");
    uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&qn,nullptr);std::vector<VkQueueFamilyProperties> qs(qn);vkGetPhysicalDeviceQueueFamilyProperties(physical,&qn,qs.data());
    for(uint32_t i=0;i<qn;++i)if(qs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){graphics=i;break;}
    compute_family=graphics;for(uint32_t i=0;i<qn;++i)if((qs[i].queueFlags&VK_QUEUE_COMPUTE_BIT)&&!(qs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){compute_family=i;break;}
    for(uint32_t family:{graphics,compute_family}){if(!queue_info.empty()&&queue_info.front().queueFamilyIndex==family)continue;VkDeviceQueueCreateInfo q {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};q.queueFamilyIndex=family;q.queueCount=1;q.pQueuePriorities=&queue_priority;queue_info.push_back(q);}
    device_info.pNext=&features;device_info.queueCreateInfoCount=queue_info.size();device_info.pQueueCreateInfos=queue_info.data();device_info.enabledExtensionCount=extensions.size();device_info.ppEnabledExtensionNames=extensions.data();
    vk(vkCreateDevice(physical,&device_info,nullptr,&device));
    pyrowave_device_create_info p {};p.GetInstanceProcAddr=vkGetInstanceProcAddr;p.instance=instance;p.physical_device=physical;p.device=device;p.instance_create_info=&instance_info;p.device_create_info=&device_info;
    pyro(pyrowave_create_device(&p,&pyrodev));set_occupancy=reinterpret_cast<PFN_vkCmdSetComputeOccupancyPriorityNV>(vkGetDeviceProcAddr(device,"vkCmdSetComputeOccupancyPriorityNV"));require(set_occupancy,"Occupancy entrypoint missing");
    vkGetDeviceQueue(device,compute_family,0,&compute_queue);require(compute_queue,"No compute queue handle");
    VkCommandPoolCreateInfo pc {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=compute_family;vk(vkCreateCommandPool(device,&pc,nullptr,&compute_pool));
  }
  explicit Device(double requested=-1,bool diagnostic=false):occupancy(requested),timestamps(diagnostic){
    try {
      if(occupancy>=0)create_custom();
      else {pyro(pyrowave_create_default_device(&pyrodev));pyrowave_device_get_vk_device_handles(pyrodev,nullptr,&physical,&device);}
      VkPhysicalDeviceProperties properties {};vkGetPhysicalDeviceProperties(physical,&properties);
      timestamp_period=properties.limits.timestampPeriod;
      require(properties.vendorID==0x10de,"Benchmark requires the intended NVIDIA GPU");
      uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);
      std::vector<VkQueueFamilyProperties> qs(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,qs.data());
      for(uint32_t i=0;i<n;++i)if(qs[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))families.push_back(i);
      auto it=std::find_if(families.begin(),families.end(),[&](uint32_t i){return bool(qs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT);});
      require(it!=families.end(),"No graphics queue");graphics=*it;vkGetDeviceQueue(device,graphics,0,&queue);require(queue,"No graphics queue handle");
      VkCommandPoolCreateInfo pc {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=graphics;
      vk(vkCreateCommandPool(device,&pc,nullptr,&pool));
      VkSemaphoreTypeCreateInfo st {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};st.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
      VkSemaphoreCreateInfo sc {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};sc.pNext=&st;vk(vkCreateSemaphore(device,&sc,nullptr,&timeline));
      if(timestamps){
        require(occupancy>=0,"GPU timestamps require the explicit owned command-buffer path");
        auto domains=reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));require(domains,"Calibratable time-domain query missing");uint32_t count=0;vk(domains(physical,&count,nullptr));std::vector<VkTimeDomainEXT> supported(count);vk(domains(physical,&count,supported.data()));require(std::find(supported.begin(),supported.end(),VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT)!=supported.end(),"Monotonic calibration domain unavailable");
        require(qs[graphics].timestampValidBits==64&&qs[compute_family].timestampValidBits==64,"Diagnostic requires64-bit queue timestamps");
        calibrated=reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(vkGetDeviceProcAddr(device,"vkGetCalibratedTimestampsEXT"));require(calibrated,"Calibrated timestamp function missing");VkQueryPoolCreateInfo qi {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;vk(vkCreateQueryPool(device,&qi,nullptr,&query));calibrate();
      }
      std::printf("{\"device\":\"%s\",\"vendor\":%u,\"vulkan_api\":%u,\"graphics_family\":%u,\"global_priority\":%u,\"explicit_command_buffer\":%s,\"compute_occupancy_priority\":%.3f}\n",properties.deviceName,properties.vendorID,properties.apiVersion,graphics,unsigned(pyrowave_device_get_global_priority(pyrodev)),occupancy>=0?"true":"false",occupancy);
    } catch(...){clear();throw;}
  }
  void clear(){if(device)vkDeviceWaitIdle(device);if(query)vkDestroyQueryPool(device,query,nullptr);if(timeline)vkDestroySemaphore(device,timeline,nullptr);if(pool)vkDestroyCommandPool(device,pool,nullptr);if(compute_pool)vkDestroyCommandPool(device,compute_pool,nullptr);if(pyrodev)pyrowave_device_destroy(pyrodev);if(owns_device){if(device)vkDestroyDevice(device,nullptr);if(instance)vkDestroyInstance(instance,nullptr);}pyrodev=nullptr;device=VK_NULL_HANDLE;}
  ~Device(){clear();}
  uint32_t memory(uint32_t bits,VkMemoryPropertyFlags flags){VkPhysicalDeviceMemoryProperties p {};vkGetPhysicalDeviceMemoryProperties(physical,&p);for(uint32_t i=0;i<p.memoryTypeCount;++i)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("Memory type unavailable");}
  VkCommandBuffer begin(bool compute=false){VkCommandBufferAllocateInfo a {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=compute?compute_pool:pool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;VkCommandBuffer c;vk(vkAllocateCommandBuffers(device,&a,&c));VkCommandBufferBeginInfo b {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vk(vkBeginCommandBuffer(c,&b));return c;}
  void wait(uint64_t value){VkSemaphoreWaitInfo w {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};w.semaphoreCount=1;w.pSemaphores=&timeline;w.pValues=&value;vk(vkWaitSemaphores(device,&w,2000000000ull));}
  void submit(VkCommandBuffer c,bool compute=false){
    vk(vkEndCommandBuffer(c));uint64_t previous=signal,value=++signal;
    VkTimelineSemaphoreSubmitInfo t {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    t.waitSemaphoreValueCount=previous?1:0;t.pWaitSemaphoreValues=&previous;t.signalSemaphoreValueCount=1;t.pSignalSemaphoreValues=&value;
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo s {VK_STRUCTURE_TYPE_SUBMIT_INFO};s.pNext=&t;s.commandBufferCount=1;s.pCommandBuffers=&c;
    s.waitSemaphoreCount=previous?1:0;s.pWaitSemaphores=&timeline;s.pWaitDstStageMask=&stage;
    s.signalSemaphoreCount=1;s.pSignalSemaphores=&timeline;
    submitted=Clock::now();vk(vkQueueSubmit(compute?compute_queue:queue,1,&s,VK_NULL_HANDLE));wait(value);woken=Clock::now();vkFreeCommandBuffers(device,compute?compute_pool:pool,1,&c);
  }
  void calibrate(){if(!timestamps)return;VkCalibratedTimestampInfoEXT info[2]{{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT},{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT}};info[0].timeDomain=VK_TIME_DOMAIN_DEVICE_EXT;info[1].timeDomain=VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;uint64_t times[2];vk(calibrated(device,2,info,times,&calibration_deviation_ns));clock_offset_ns=double(times[1])-double(times[0])*timestamp_period;calibrated_at=Clock::now();}
  template<class F>void record(bool compute,F &&f){
    auto c=begin(compute);VkComputeOccupancyPriorityParametersNV parameters {VK_STRUCTURE_TYPE_COMPUTE_OCCUPANCY_PRIORITY_PARAMETERS_NV};parameters.occupancyPriority=occupancy;parameters.occupancyThrottling=0;
    if(timestamps){vkCmdResetQueryPool(c,query,0,2);vkCmdWriteTimestamp2(c,VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,query,0);}
    set_occupancy(c,&parameters);pyrowave_device_set_command_buffer(pyrodev,c);
    try{f();}catch(...){pyrowave_device_set_command_buffer(pyrodev,VK_NULL_HANDLE);throw;}
    pyrowave_device_set_command_buffer(pyrodev,VK_NULL_HANDLE);if(timestamps)vkCmdWriteTimestamp2(c,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,query,1);submit(c,compute);
    if(timestamps){uint64_t times[2];vk(vkGetQueryPoolResults(device,query,0,2,sizeof(times),times,8,VK_QUERY_RESULT_64_BIT));require(times[1]>=times[0],"Reversed GPU timestamps");last_gpu_us=(times[1]-times[0])*timestamp_period/1000.;double gpu_start=double(times[0])*timestamp_period+clock_offset_ns,gpu_end=double(times[1])*timestamp_period+clock_offset_ns;double cpu_start=std::chrono::duration<double,std::nano>(submitted.time_since_epoch()).count(),cpu_end=std::chrono::duration<double,std::nano>(woken.time_since_epoch()).count();last_queue_us=(gpu_start-cpu_start)/1000.;last_notify_us=(cpu_end-gpu_end)/1000.;}
  }
};
struct Buffer {
  Device &d;VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void *mapped=nullptr;
  Buffer(Device &d,size_t n):d(d){try{VkBufferCreateInfo b {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.size=n;b.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;vk(vkCreateBuffer(d.device,&b,nullptr,&buffer));VkMemoryRequirements r;vkGetBufferMemoryRequirements(d.device,buffer,&r);VkMemoryAllocateInfo a {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=d.memory(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);vk(vkAllocateMemory(d.device,&a,nullptr,&memory));vk(vkBindBufferMemory(d.device,buffer,memory,0));vk(vkMapMemory(d.device,memory,0,VK_WHOLE_SIZE,0,&mapped));}catch(...){clear();throw;}}
  void clear(){if(mapped)vkUnmapMemory(d.device,memory);if(buffer)vkDestroyBuffer(d.device,buffer,nullptr);if(memory)vkFreeMemory(d.device,memory,nullptr);}
  ~Buffer(){clear();}
};
struct Plane {
  Device &d;unsigned w,h;VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
  Plane(Device &d,unsigned w,unsigned h,const std::vector<unsigned char> &source):d(d),w(w),h(h){
    try{
      VkImageCreateInfo i {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};i.imageType=VK_IMAGE_TYPE_2D;i.format=VK_FORMAT_R8_UNORM;i.extent={w,h,1};i.mipLevels=i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;
      i.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      i.sharingMode=d.families.size()>1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;i.queueFamilyIndexCount=d.families.size();i.pQueueFamilyIndices=d.families.data();
      vk(vkCreateImage(d.device,&i,nullptr,&image));VkMemoryRequirements r;vkGetImageMemoryRequirements(d.device,image,&r);VkMemoryAllocateInfo a {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=d.memory(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);vk(vkAllocateMemory(d.device,&a,nullptr,&memory));vk(vkBindImageMemory(d.device,image,memory,0));
      Buffer upload(d,size_t(w)*h);if(!source.empty()){require(source.size()==size_t(w)*h,"Wrong plane length");std::memcpy(upload.mapped,source.data(),source.size());}else std::memset(upload.mapped,0,size_t(w)*h);
      auto c=d.begin();barrier(c,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
      VkBufferImageCopy copy {};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};vkCmdCopyBufferToImage(c,upload.buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
      barrier(c,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);d.submit(c);
    }catch(...){clear();throw;}
  }
  void clear(){if(image)vkDestroyImage(d.device,image,nullptr);if(memory)vkFreeMemory(d.device,memory,nullptr);}
  ~Plane(){clear();}
  void barrier(VkCommandBuffer c,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags ss,VkPipelineStageFlags ds){VkImageMemoryBarrier b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=old;b.newLayout=next;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(c,ss,ds,0,0,nullptr,0,nullptr,1,&b);}
  pyrowave_image_view view()const{pyrowave_image_view v {};v.image=image;v.width=w;v.height=h;v.image_format=v.view_format=VK_FORMAT_R8_UNORM;v.aspect=VK_IMAGE_ASPECT_COLOR_BIT;v.swizzle=VK_COMPONENT_SWIZZLE_IDENTITY;v.layout=VK_IMAGE_LAYOUT_GENERAL;return v;}
  std::vector<unsigned char> read(){Buffer b(d,size_t(w)*h);auto c=d.begin();barrier(c,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);VkBufferImageCopy copy {};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};vkCmdCopyImageToBuffer(c,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b.buffer,1,&copy);barrier(c,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);d.submit(c);auto p=static_cast<unsigned char *>(b.mapped);return {p,p+size_t(w)*h};}
};
static unsigned hash(unsigned x,unsigned y){unsigned v=x*0x45d9f3bu^y*0x119de1f3u;v^=v>>16;v*=0x45d9f3bu;v^=v>>16;return v&255;}
// Same captured-pixel-style fixture as PixelFrames, expressed in planar YUV.
static std::array<std::vector<unsigned char>,3> source(unsigned w,unsigned h,unsigned index){
  std::array<std::vector<unsigned char>,3> p;p[0].resize(size_t(w)*h);p[1].resize(size_t(w)*h/4);p[2].resize(size_t(w)*h/4);
  unsigned dx=index*4,dy=index*2;
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){unsigned sx=(x+w-dx)%w,sy=(y+h-dy)%h,xx=sx/32,yy=sy/32,fx=sx%32,fy=sy%32;unsigned top=hash(xx,yy)*(32-fx)+hash(xx+1,yy)*fx,bottom=hash(xx,yy+1)*(32-fx)+hash(xx+1,yy+1)*fx,value=(top*(32-fy)+bottom*fy+512)/1024;if(sx%83<2||sy%61<2)value=230;p[0][size_t(y)*w+x]=16+(219*value+127)/255;}
  for(unsigned y=0;y<h/2;++y)for(unsigned x=0;x<w/2;++x){unsigned sx=(x+w/2-dx/2)%(w/2),sy=(y+h/2-dy/2)%(h/2);p[1][size_t(y)*w/2+x]=32+(sx/24+sy/17)%193;p[2][size_t(y)*w/2+x]=32+(sx/33+sy/23)%193;}
  return p;
}
struct Codec {
  pyrowave_encoder encoder=nullptr;pyrowave_decoder decoder=nullptr;
  Codec(Device &d,unsigned w,unsigned h,bool fragment){try{pyrowave_encoder_create_info e {};e.device=d.pyrodev;e.width=w;e.height=h;e.chroma=PYROWAVE_CHROMA_SUBSAMPLING_420;pyro(pyrowave_encoder_create(&e,&encoder));pyrowave_decoder_create_info f {};f.device=d.pyrodev;f.width=w;f.height=h;f.chroma=e.chroma;f.fragment_path=fragment;pyro(pyrowave_decoder_create(&f,&decoder));}catch(...){clear();throw;}}
  void clear(){if(decoder)pyrowave_decoder_destroy(decoder);if(encoder)pyrowave_encoder_destroy(encoder);decoder=nullptr;encoder=nullptr;}
  ~Codec(){clear();}
};
static void run(Device &d,Telemetry &telemetry,unsigned w,unsigned h,unsigned mbps,bool compute,bool fragment,int round){
  using Planes=std::array<std::unique_ptr<Plane>,3>;
  std::array<Planes,4> images;std::array<std::array<std::vector<unsigned char>,3>,4> reference;
  Planes out;pyrowave_gpu_buffers dst {};
  for(unsigned p=0;p<3;++p){out[p]=std::make_unique<Plane>(d,p?w/2:w,p?h/2:h,std::vector<unsigned char>{});dst.planes[p]=out[p]->view();}
  for(unsigned frame=0;frame<4;++frame){reference[frame]=source(w,h,frame);for(unsigned p=0;p<3;++p){images[frame][p]=std::make_unique<Plane>(d,p?w/2:w,p?h/2:h,reference[frame][p]);require(images[frame][p]->read()==reference[frame][p],"Uploaded source mismatch");}}
  const uint64_t ready=d.signal;
  Codec codec(d,w,h,fragment);
  pyrowave_rate_control rate {size_t(mbps)*1000000/(120*8)};
  std::vector<unsigned char> bitstream(rate.maximum_bitstream_size+1024*1024);
  std::vector<pyrowave_packet> packets(16384);
  std::vector<double> encode,packetize,decode,total,encode_gpu,encode_queue,encode_notify,decode_gpu;std::array<double,3> mae {},mse {};std::array<unsigned,3> maximum {};
  size_t packet_bytes=0,num_packets_sum=0;unsigned quality_frames=0,warm_frames=0;unsigned clock_before=telemetry.mhz();
  auto warm_deadline=Clock::now()+std::chrono::milliseconds(200);unsigned measured=0;
  for(unsigned frame=0;measured<50;++frame){
    unsigned index=frame%4;pyrowave_gpu_buffers src {};for(unsigned p=0;p<3;++p)src.planes[p]=images[index][p]->view();
    pyro(pyrowave_device_set_queue_type(d.pyrodev,compute ? VK_QUEUE_COMPUTE_BIT : VK_QUEUE_GRAPHICS_BIT));
    pyrowave_gpu_sync_operation acquire {};acquire.sync={d.timeline,ready};
    if(d.timestamps && Clock::now()-d.calibrated_at>std::chrono::seconds(1))d.calibrate();
    auto begin=Clock::now();
    if(d.occupancy>=0)d.record(compute,[&]{pyro(pyrowave_encoder_encode_gpu(codec.encoder,nullptr,nullptr,&src,&rate));});
    else pyro(pyrowave_encoder_encode_gpu(codec.encoder,&acquire,nullptr,&src,&rate));
    double gpu_encode=d.last_gpu_us,queue_encode=d.last_queue_us,notify_encode=d.last_notify_us;
    size_t count=0;pyro(pyrowave_encoder_compute_num_packets(codec.encoder,1200,&count));auto completed=Clock::now();
    require(count<=packets.size(),"Too many packets");pyro(pyrowave_encoder_packetize(codec.encoder,packets.data(),1200,&count,bitstream.data(),bitstream.size()));auto packed=Clock::now();
    size_t bytes=0;pyrowave_decoder_clear(codec.decoder);
    for(size_t i=0;i<count;++i){require(packets[i].offset+packets[i].size<=bitstream.size(),"Packet out of bounds");bytes+=packets[i].size;pyro(pyrowave_decoder_push_packet(codec.decoder,bitstream.data()+packets[i].offset,packets[i].size));}
    require(pyrowave_decoder_decode_is_ready(codec.decoder,false),"Decoder not ready after all packets");
    pyro(pyrowave_device_set_queue_type(d.pyrodev,fragment ? VK_QUEUE_GRAPHICS_BIT : compute ? VK_QUEUE_COMPUTE_BIT : VK_QUEUE_GRAPHICS_BIT));
    if(d.occupancy>=0)d.record(!fragment && compute,[&]{pyro(pyrowave_decoder_decode_gpu_buffer(codec.decoder,nullptr,nullptr,&dst));});
    else {
      pyrowave_gpu_sync_operation release {};uint64_t done=++d.signal;release.sync={d.timeline,done};
      pyrowave_gpu_sync_operation decode_acquire {};decode_acquire.sync={d.timeline,done-1};
      pyro(pyrowave_decoder_decode_gpu_buffer(codec.decoder,&decode_acquire,&release,&dst));d.wait(done);
    }
    auto decoded=Clock::now();
    bool measuring=frame>=10 && begin>=warm_deadline;
    if(measuring){encode.push_back(us(begin,completed));packetize.push_back(us(begin,packed));decode.push_back(us(packed,decoded));total.push_back(us(begin,decoded));packet_bytes+=bytes;num_packets_sum+=count;++measured;if(d.timestamps){encode_gpu.push_back(gpu_encode);encode_queue.push_back(queue_encode);encode_notify.push_back(notify_encode);decode_gpu.push_back(d.last_gpu_us);}}else ++warm_frames;
    // Independent pixel readback outside latency spans verifies the final four moving frames.
    if(measuring && measured>=47){for(unsigned p=0;p<3;++p){auto actual=out[p]->read();const auto &expected=reference[index][p];double a=0,s=0;for(size_t i=0;i<actual.size();++i){int error=int(actual[i])-int(expected[i]);a+=std::abs(error);s+=error*error;maximum[p]=std::max(maximum[p],unsigned(std::abs(error)));}mae[p]+=a/actual.size();mse[p]+=s/actual.size();}++quality_frames;}
  }
  std::printf("{\"width\":%u,\"height\":%u,\"target_mbps\":%u,\"queue\":\"%s\",\"decoder\":\"%s\",\"round\":%d,\"samples\":50,\"warmup_ms_min\":200,\"warmup_frames\":%u,\"decoded_frames\":%u,\"clock_before_mhz\":%u,\"clock_after_mhz\":%u,\"target_payload_bytes\":%zu,\"mean_packet_bytes\":%.3f,\"packet_mbps_at_120\":%.3f,\"mean_packets\":%.3f,\"completed_encode_us\":%s,\"cpu_packets_ready_us\":%s,\"cpu_packets_to_gpu_decode_us\":%s,\"local_roundtrip_us\":%s,\"quality_frames\":%u,\"luma_mae\":%.5f,\"cb_mae\":%.5f,\"cr_mae\":%.5f,\"luma_psnr_db\":%.3f,\"luma_max_error\":%u",w,h,mbps,compute?"compute":"graphics",fragment?"fragment":"compute",round,warm_frames,warm_frames+measured,clock_before,telemetry.mhz(),rate.maximum_bitstream_size,packet_bytes/50.,packet_bytes*8.*120/50/1e6,num_packets_sum/50.,quantiles(encode).c_str(),quantiles(packetize).c_str(),quantiles(decode).c_str(),quantiles(total).c_str(),quality_frames,mae[0]/quality_frames,mae[1]/quality_frames,mae[2]/quality_frames,mse[0]?10*std::log10(255.*255*quality_frames/mse[0]):999.,maximum[0]);
  if(d.timestamps)std::printf(",\"instrumented\":true,\"calibration_max_deviation_ns\":%llu,\"encode_gpu_us\":%s,\"encode_queue_delay_us\":%s,\"encode_notification_us\":%s,\"decode_gpu_us\":%s",static_cast<unsigned long long>(d.calibration_deviation_ns),quantiles(encode_gpu).c_str(),quantiles(encode_queue).c_str(),quantiles(encode_notify).c_str(),quantiles(decode_gpu).c_str());
  std::puts("}");std::fflush(stdout);
}
int main(int argc,char **argv){
  try {
    double occupancy=-1;bool timestamps=false,only_4k=false;unsigned selected_rate=0;
    for(int i=1;i<argc;++i){std::string arg=argv[i];
      if(arg=="--occupancy" && i+1<argc){std::string value=argv[++i];require(value=="0"||value=="0.5"||value=="1","Occupancy must be0,0.5or1");occupancy=std::stod(value);}
      else if(arg=="--gpu-timestamps")timestamps=true;
      else if(arg=="--only-4k")only_4k=true;
      else if(arg=="--rate" && i+1<argc){std::string value=argv[++i];require(value=="23"||value=="100"||value=="200"||value=="400","Unsupported rate");selected_rate=std::stoul(value);}
      else throw std::runtime_error("Usage: latency-probe [--occupancy0|0.5|1] [--gpu-timestamps] [--only-4k] [--rate23|100|200|400]");
    }
    require(!timestamps||occupancy>=0,"GPU timestamps require an explicit occupancy control");
    Device d(occupancy,timestamps);Telemetry telemetry;const unsigned rates[]={23,100,200,400};
    for(int round=0;round<2;++round)for(unsigned geometry=only_4k?1:0;geometry<2;++geometry)for(unsigned step=0;step<4;++step)for(unsigned path=0;path<2;++path){
      unsigned rate=rates[round?3-step:step];if(selected_rate && rate!=selected_rate)continue;
      bool compute=bool(round?1-path:path);run(d,telemetry,geometry?3840:1920,geometry?2160:1080,rate,compute,false,round);
    }
    return 0;
  }catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
