// SPDX-License-Identifier: GPL-3.0-only
/** @file vulkan_shared_surface.h
 * @brief Owned Vulkan producer and persistent CUDA view; no game interception.
 * Requires the native probe's checked cu()/require() helpers and a current CUDA context.
 */
#pragma once
#include <vulkan/vulkan.h>
#include <fstream>
#include <unistd.h>

/** @brief Treat unsupported external sharing as an explicit failed experiment. */
static void vk_checked(VkResult result, const char *operation) {
  if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": Vulkan "+std::to_string(result));
}

/** @brief Keep image, imported view and two ownership semaphores alive together. */
struct VulkanSharedSurface {
  VkInstance instance=VK_NULL_HANDLE;
  VkPhysicalDevice physical=VK_NULL_HANDLE;
  VkDevice device=VK_NULL_HANDLE;
  VkQueue queue=VK_NULL_HANDLE;
  uint32_t family=0;
  VkImage image=VK_NULL_HANDLE;
  VkDeviceMemory memory=VK_NULL_HANDLE;
  VkImageView view=VK_NULL_HANDLE;
  VkSemaphore ready=VK_NULL_HANDLE, reusable=VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool=VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout=VK_NULL_HANDLE;
  VkPipeline pipeline=VK_NULL_HANDLE;
  VkShaderModule shader=VK_NULL_HANDLE;
  VkCommandPool command_pool=VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> commands;
  CUexternalMemory imported_memory=nullptr;
  CUmipmappedArray imported_mipmap=nullptr;
  CUarray array=nullptr;
  CUexternalSemaphore cuda_ready=nullptr, cuda_reusable=nullptr;

  /** @brief Complete owned work before destroying shared views and original memory. */
  ~VulkanSharedSurface() {
    if(device) {cuCtxSynchronize();vkDeviceWaitIdle(device);}
    if(cuda_ready) cuDestroyExternalSemaphore(cuda_ready);
    if(cuda_reusable) cuDestroyExternalSemaphore(cuda_reusable);
    if(imported_mipmap) cuMipmappedArrayDestroy(imported_mipmap);
    if(imported_memory) cuDestroyExternalMemory(imported_memory);
    if(device) {
      if(command_pool) vkDestroyCommandPool(device,command_pool,nullptr);
      if(pipeline) vkDestroyPipeline(device,pipeline,nullptr);
      if(shader) vkDestroyShaderModule(device,shader,nullptr);
      if(pipeline_layout) vkDestroyPipelineLayout(device,pipeline_layout,nullptr);
      if(descriptor_pool) vkDestroyDescriptorPool(device,descriptor_pool,nullptr);
      if(descriptors) vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
      if(ready) vkDestroySemaphore(device,ready,nullptr);
      if(reusable) vkDestroySemaphore(device,reusable,nullptr);
      if(view) vkDestroyImageView(device,view,nullptr);
      if(image) vkDestroyImage(device,image,nullptr);
      if(memory) vkFreeMemory(device,memory,nullptr);
      vkDestroyDevice(device,nullptr);
    }
    if(instance) vkDestroyInstance(instance,nullptr);
  }

  /** @brief Match Vulkan and CUDA UUIDs and validate external image export support. */
  void init(unsigned width, unsigned height, const std::filesystem::path &shader_path) {
    VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="owned-shared-surface-probe";app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance_info.pApplicationInfo=&app;
    vk_checked(vkCreateInstance(&instance_info,nullptr,&instance),"create instance");
    CUdevice cuda_device; CUuuid uuid;cu(cuCtxGetDevice(&cuda_device));cu(cuDeviceGetUuid(&uuid,cuda_device));
    uint32_t count=0;vk_checked(vkEnumeratePhysicalDevices(instance,&count,nullptr),"count devices");
    std::vector<VkPhysicalDevice> devices(count);vk_checked(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"enumerate devices");
    for(auto candidate:devices) {
      VkPhysicalDeviceIDProperties id {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 properties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&id;
      vkGetPhysicalDeviceProperties2(candidate,&properties);
      if(std::memcmp(id.deviceUUID,uuid.bytes,VK_UUID_SIZE)==0) {physical=candidate;break;}
    }
    require(physical!=VK_NULL_HANDLE,"No Vulkan device matches the CUDA device UUID");
    vk_checked(vkEnumeratePhysicalDeviceGroups(instance,&count,nullptr),"count device groups");
    std::vector<VkPhysicalDeviceGroupProperties> groups(count);
    for(auto &group:groups) group.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
    vk_checked(vkEnumeratePhysicalDeviceGroups(instance,&count,groups.data()),"enumerate device groups");
    for(const auto &group:groups) for(uint32_t i=0;i<group.physicalDeviceCount;++i)
      if(group.physicalDevices[i]==physical) require(group.physicalDeviceCount==1,"External CUDA sharing requires a single-device Vulkan group");
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(physical,&features);
    require(features.shaderStorageImageExtendedFormats,"R8 storage image feature unavailable");
    VkPhysicalDeviceExternalImageFormatInfo external {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};external.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkPhysicalDeviceImageFormatInfo2 format {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};format.pNext=&external;
    format.format=VK_FORMAT_R8_UINT;format.type=VK_IMAGE_TYPE_2D;format.tiling=VK_IMAGE_TILING_OPTIMAL;format.usage=VK_IMAGE_USAGE_STORAGE_BIT;
    VkExternalImageFormatProperties external_properties {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};properties.pNext=&external_properties;
    vk_checked(vkGetPhysicalDeviceImageFormatProperties2(physical,&format,&properties),"query external R8 image");
    require(external_properties.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT,"Image is not externally exportable");
    require(width<=properties.imageFormatProperties.maxExtent.width && height*3/2<=properties.imageFormatProperties.maxExtent.height,"Shared image exceeds supported extent");
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
    for(family=0;family<count;++family)if((families[family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))break;
    require(family<count,"No graphics/compute producer queue");
    float priority=1;VkDeviceQueueCreateInfo queue_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue_info.queueFamilyIndex=family;queue_info.queueCount=1;queue_info.pQueuePriorities=&priority;
    const char *extensions[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    VkPhysicalDeviceFeatures enabled {};enabled.shaderStorageImageExtendedFormats=VK_TRUE;
    VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device_info.queueCreateInfoCount=1;device_info.pQueueCreateInfos=&queue_info;
    device_info.enabledExtensionCount=2;device_info.ppEnabledExtensionNames=extensions;device_info.pEnabledFeatures=&enabled;
    vk_checked(vkCreateDevice(physical,&device_info,nullptr,&device),"create device");vkGetDeviceQueue(device,family,0,&queue);
    VkExternalMemoryImageCreateInfo external_image {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external_image.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image_info.pNext=&external_image;image_info.imageType=VK_IMAGE_TYPE_2D;
    image_info.format=VK_FORMAT_R8_UINT;image_info.extent={width,height*3/2,1};image_info.mipLevels=1;image_info.arrayLayers=1;
    image_info.samples=VK_SAMPLE_COUNT_1_BIT;image_info.tiling=VK_IMAGE_TILING_OPTIMAL;image_info.usage=VK_IMAGE_USAGE_STORAGE_BIT;image_info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vk_checked(vkCreateImage(device,&image_info,nullptr,&image),"create shared image");
    VkMemoryRequirements requirements;vkGetImageMemoryRequirements(device,image,&requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
    uint32_t memory_type=0;for(;memory_type<memory_properties.memoryTypeCount;++memory_type)
      if((requirements.memoryTypeBits&(1u<<memory_type)) && (memory_properties.memoryTypes[memory_type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))break;
    require(memory_type<memory_properties.memoryTypeCount,"No local shared image memory");
    VkExportMemoryAllocateInfo export_info {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};export_info.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryDedicatedAllocateInfo dedicated {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=image;dedicated.pNext=&export_info;
    VkMemoryAllocateInfo allocation {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.pNext=&dedicated;allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memory_type;
    vk_checked(vkAllocateMemory(device,&allocation,nullptr,&memory),"allocate shared image");vk_checked(vkBindImageMemory(device,image,memory,0),"bind shared image");
    auto memory_fd=reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device,"vkGetMemoryFdKHR"));require(memory_fd,"No external-memory FD entry point");
    VkMemoryGetFdInfoKHR fd_info {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};fd_info.memory=memory;fd_info.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd=-1;vk_checked(memory_fd(device,&fd_info,&fd),"export shared image");
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC cuda_memory {};cuda_memory.type=CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;cuda_memory.handle.fd=fd;
    cuda_memory.size=requirements.size;cuda_memory.flags=CUDA_EXTERNAL_MEMORY_DEDICATED;
    auto imported=cuImportExternalMemory(&imported_memory,&cuda_memory);if(imported!=CUDA_SUCCESS)close(fd);cu(imported);
    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mapped {};mapped.numLevels=1;
    mapped.arrayDesc.Width=width;mapped.arrayDesc.Height=height*3/2;mapped.arrayDesc.Format=CU_AD_FORMAT_UNSIGNED_INT8;
    mapped.arrayDesc.NumChannels=1;mapped.arrayDesc.Flags=CUDA_ARRAY3D_SURFACE_LDST;
    cu(cuExternalMemoryGetMappedMipmappedArray(&imported_mipmap,imported_memory,&mapped));cu(cuMipmappedArrayGetLevel(&array,imported_mipmap,0));
    CUDA_ARRAY3D_DESCRIPTOR actual {};cu(cuArray3DGetDescriptor(&actual,array));
    require(actual.Width==width && actual.Height==height*3/2 && actual.Depth==0 && actual.NumChannels==1 && actual.Format==CU_AD_FORMAT_UNSIGNED_INT8 && (actual.Flags&CUDA_ARRAY3D_SURFACE_LDST),"Imported array violates encoder input contract");
    create_semaphore(ready,cuda_ready);create_semaphore(reusable,cuda_reusable);
    create_pipeline(width,height,shader_path);
  }

  /** @brief Export a binary ownership semaphore; CUDA owns its FD after successful import. */
  void create_semaphore(VkSemaphore &semaphore, CUexternalSemaphore &imported) {
    VkExportSemaphoreCreateInfo external {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo info {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};info.pNext=&external;
    vk_checked(vkCreateSemaphore(device,&info,nullptr,&semaphore),"create ownership semaphore");
    auto get_fd=reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device,"vkGetSemaphoreFdKHR"));require(get_fd,"No semaphore FD entry point");
    VkSemaphoreGetFdInfoKHR fd_info {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};fd_info.semaphore=semaphore;fd_info.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd=-1;vk_checked(get_fd(device,&fd_info,&fd),"export ownership semaphore");
    CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC handle {};handle.type=CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;handle.handle.fd=fd;
    auto result=cuImportExternalSemaphore(&imported,&handle);if(result!=CUDA_SUCCESS)close(fd);cu(result);
  }

  /** @brief Record each frame once, avoiding command-buffer reuse while external work is pending. */
  void create_pipeline(unsigned width,unsigned height,const std::filesystem::path &shader_path) {
    VkImageViewCreateInfo image_view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};image_view.image=image;image_view.viewType=VK_IMAGE_VIEW_TYPE_2D;
    image_view.format=VK_FORMAT_R8_UINT;image_view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vk_checked(vkCreateImageView(device,&image_view,nullptr,&view),"create producer image view");
    constexpr unsigned image_count=1;
    VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo descriptor_info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};descriptor_info.bindingCount=image_count;descriptor_info.pBindings=bindings;
    vk_checked(vkCreateDescriptorSetLayout(device,&descriptor_info,nullptr,&descriptors),"create descriptors");
    VkDescriptorPoolSize size {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,image_count};VkDescriptorPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool_info.maxSets=1;pool_info.poolSizeCount=1;pool_info.pPoolSizes=&size;
    vk_checked(vkCreateDescriptorPool(device,&pool_info,nullptr,&descriptor_pool),"create descriptor pool");
    VkDescriptorSetAllocateInfo allocate {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=descriptor_pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&descriptors;
    VkDescriptorSet set;vk_checked(vkAllocateDescriptorSets(device,&allocate,&set),"allocate producer descriptors");
    VkDescriptorImageInfo image_descriptors[1] {};VkWriteDescriptorSet writes[1] {};
    for(unsigned i=0;i<image_count;++i) {
      image_descriptors[i].imageView=view;image_descriptors[i].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
      writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&image_descriptors[i];
    }
    vkUpdateDescriptorSets(device,image_count,writes,0,nullptr);
    VkPushConstantRange range {VK_SHADER_STAGE_COMPUTE_BIT,0,12};VkPipelineLayoutCreateInfo layout_info {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout_info.setLayoutCount=1;layout_info.pSetLayouts=&descriptors;layout_info.pushConstantRangeCount=1;layout_info.pPushConstantRanges=&range;
    vk_checked(vkCreatePipelineLayout(device,&layout_info,nullptr,&pipeline_layout),"create pipeline layout");
    std::ifstream input(shader_path,std::ios::binary|std::ios::ate);require(bool(input),"Open shared producer shader failed");
    auto bytes=input.tellg();require(bytes>0 && uint64_t(bytes)%4==0,"Invalid SPIR-V size");std::vector<uint32_t> code(size_t(bytes)/4);
    input.seekg(0);input.read(reinterpret_cast<char *>(code.data()),bytes);require(bool(input),"Read shared producer shader failed");
    VkShaderModuleCreateInfo shader_info {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader_info.codeSize=size_t(bytes);shader_info.pCode=code.data();vk_checked(vkCreateShaderModule(device,&shader_info,nullptr,&shader),"create producer shader");
    VkComputePipelineCreateInfo pipeline_info {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline_info.layout=pipeline_layout;
    pipeline_info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pipeline_info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline_info.stage.module=shader;pipeline_info.stage.pName="main";
    vk_checked(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_info,nullptr,&pipeline),"create producer pipeline");
    VkCommandPoolCreateInfo command_info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};command_info.queueFamilyIndex=family;vk_checked(vkCreateCommandPool(device,&command_info,nullptr,&command_pool),"create producer command pool");
    commands.resize(120);VkCommandBufferAllocateInfo command_allocate {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command_allocate.commandPool=command_pool;command_allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command_allocate.commandBufferCount=commands.size();
    vk_checked(vkAllocateCommandBuffers(device,&command_allocate,commands.data()),"allocate producer commands");
    for(unsigned frame=0;frame<commands.size();++frame) {
      auto command=commands[frame];VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vk_checked(vkBeginCommandBuffer(command,&begin),"begin producer frame");
      VkImageMemoryBarrier acquire {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};acquire.oldLayout=frame ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;acquire.newLayout=VK_IMAGE_LAYOUT_GENERAL;
      acquire.srcQueueFamilyIndex=frame ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;acquire.dstQueueFamilyIndex=frame ? family : VK_QUEUE_FAMILY_IGNORED;
      acquire.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;acquire.image=image;acquire.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&acquire);
      vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);
      uint32_t constants[]={width,height,frame};vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(constants),constants);
      vkCmdDispatch(command,(width+31)/32,(height*3/2+7)/8,1);
      VkImageMemoryBarrier release=acquire;release.oldLayout=release.newLayout=VK_IMAGE_LAYOUT_GENERAL;release.srcQueueFamilyIndex=family;release.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;release.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;release.dstAccessMask=0;
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&release);
      vk_checked(vkEndCommandBuffer(command),"end producer frame");
    }
  }

  /** @brief Hand the same allocation from the producer queue to the encoder's input stream. */
  void produce(unsigned frame,CUstream stream) {
    require(frame<commands.size(),"Shared producer frame out of range");
    // Without maintenance8, ownership acquisition must wait at ALL_COMMANDS.
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&commands[frame];submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&ready;
    if(frame) {submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&reusable;submit.pWaitDstStageMask=&stage;}
    vk_checked(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE),"submit shared producer");
    CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wait {};cu(cuWaitExternalSemaphoresAsync(&cuda_ready,&wait,1,stream));
  }

  /** @brief Permit the next producer write only after this input has been consumed. */
  void retire(CUstream stream) {
    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal {};cu(cuSignalExternalSemaphoresAsync(&cuda_reusable,&signal,1,stream));
  }
};

/**
 * @brief Two Vulkan-exported binary semaphores order persistent GL/EGL/CUDA image access.
 * This owns no game or display resource. CUDA waits for each GL write and signals
 * read completion before GL may overwrite the same image on the next iteration.
 */
struct GlCudaSemaphoreBridge {
  VkInstance instance=VK_NULL_HANDLE;
  VkPhysicalDevice physical=VK_NULL_HANDLE;
  VkDevice device=VK_NULL_HANDLE;
  std::array<VkSemaphore,2> semaphores {};
  std::array<CUexternalSemaphore,2> cuda_semaphores {};
  std::array<GLuint,2> gl_semaphores {};
  PFNGLGENSEMAPHORESEXTPROC generate=nullptr;
  PFNGLDELETESEMAPHORESEXTPROC destroy=nullptr;
  PFNGLIMPORTSEMAPHOREFDEXTPROC import=nullptr;
  PFNGLSIGNALSEMAPHOREEXTPROC signal=nullptr;
  PFNGLWAITSEMAPHOREEXTPROC wait=nullptr;

  /** @brief Complete submitted access before dropping either API's semaphore reference. */
  ~GlCudaSemaphoreBridge() {
    if(device) {cuCtxSynchronize();glFinish();}
    for(auto semaphore:cuda_semaphores)if(semaphore)cuDestroyExternalSemaphore(semaphore);
    if(destroy)for(auto semaphore:gl_semaphores)if(semaphore)destroy(1,&semaphore);
    if(device) {
      for(auto semaphore:semaphores)if(semaphore)vkDestroySemaphore(device,semaphore,nullptr);
      vkDestroyDevice(device,nullptr);
    }
    if(instance)vkDestroyInstance(instance,nullptr);
  }
  /** @brief Validate both advertised GL extensions, not just globally resolvable entry points. */
  static bool extension(const char *name) {
    GLint count=0;glGetIntegerv(GL_NUM_EXTENSIONS,&count);
    for(GLint i=0;i<count;++i)if(std::strcmp(reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS,i)),name)==0)return true;
    return false;
  }
  /** @brief Match the current CUDA device and export one binary semaphore to each consumer API. */
  void init() {
    require(extension("GL_EXT_semaphore")&&extension("GL_EXT_semaphore_fd"),"GL external semaphore extensions unavailable");
    generate=reinterpret_cast<PFNGLGENSEMAPHORESEXTPROC>(eglGetProcAddress("glGenSemaphoresEXT"));
    destroy=reinterpret_cast<PFNGLDELETESEMAPHORESEXTPROC>(eglGetProcAddress("glDeleteSemaphoresEXT"));
    import=reinterpret_cast<PFNGLIMPORTSEMAPHOREFDEXTPROC>(eglGetProcAddress("glImportSemaphoreFdEXT"));
    signal=reinterpret_cast<PFNGLSIGNALSEMAPHOREEXTPROC>(eglGetProcAddress("glSignalSemaphoreEXT"));
    wait=reinterpret_cast<PFNGLWAITSEMAPHOREEXTPROC>(eglGetProcAddress("glWaitSemaphoreEXT"));
    require(generate&&destroy&&import&&signal&&wait,"GL external semaphore entry points unavailable");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="owned-gl-cuda-handoff";app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&app;
    vk_checked(vkCreateInstance(&create,nullptr,&instance),"create semaphore bridge instance");
    CUdevice cuda_device;CUuuid uuid;cu(cuCtxGetDevice(&cuda_device));cu(cuDeviceGetUuid(&uuid,cuda_device));
    uint32_t count=0;vk_checked(vkEnumeratePhysicalDevices(instance,&count,nullptr),"count bridge devices");
    std::vector<VkPhysicalDevice> devices(count);vk_checked(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"enumerate bridge devices");
    for(auto candidate:devices){
      VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&id;vkGetPhysicalDeviceProperties2(candidate,&properties);
      if(std::memcmp(id.deviceUUID,uuid.bytes,VK_UUID_SIZE)==0){physical=candidate;break;}
    }
    require(physical!=VK_NULL_HANDLE,"No Vulkan bridge device matches CUDA UUID");
    VkPhysicalDeviceExternalSemaphoreInfo semaphore_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};semaphore_info.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};vkGetPhysicalDeviceExternalSemaphoreProperties(physical,&semaphore_info,&properties);
    require(properties.externalSemaphoreFeatures&VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT,"Binary semaphore opaque FD export unsupported");
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
    uint32_t family=0;for(;family<count;++family)if(families[family].queueCount)break;require(family<count,"No bridge device queue");
    float priority=1;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=family;queue.queueCount=1;queue.pQueuePriorities=&priority;
    const char *extensions[]={VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device_info.queueCreateInfoCount=1;device_info.pQueueCreateInfos=&queue;device_info.enabledExtensionCount=1;device_info.ppEnabledExtensionNames=extensions;
    vk_checked(vkCreateDevice(physical,&device_info,nullptr,&device),"create semaphore bridge device");
    auto export_fd=reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device,"vkGetSemaphoreFdKHR"));require(export_fd,"Semaphore bridge export API unavailable");
    for(unsigned i=0;i<2;++i){
      VkExportSemaphoreCreateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};export_info.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};info.pNext=&export_info;vk_checked(vkCreateSemaphore(device,&info,nullptr,&semaphores[i]),"create bridge semaphore");
      VkSemaphoreGetFdInfoKHR fd_info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};fd_info.semaphore=semaphores[i];fd_info.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      int fd=-1;vk_checked(export_fd(device,&fd_info,&fd),"export CUDA bridge semaphore");
      CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC cuda_info{};cuda_info.type=CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;cuda_info.handle.fd=fd;
      auto status=cuImportExternalSemaphore(&cuda_semaphores[i],&cuda_info);if(status!=CUDA_SUCCESS)close(fd);cu(status);
      vk_checked(export_fd(device,&fd_info,&fd),"export GL bridge semaphore");
      generate(1,&gl_semaphores[i]);
      if(glGetError()!=GL_NO_ERROR){close(fd);throw std::runtime_error("Generate GL bridge semaphore failed");}
      import(gl_semaphores[i],GL_HANDLE_TYPE_OPAQUE_FD_EXT,fd);
      if(glGetError()!=GL_NO_ERROR){close(fd);throw std::runtime_error("Import GL bridge semaphore failed");}
      // Successful imports consume their separate exported file descriptors.
    }
  }
  /** @brief Queue the next GL overwrite only after the previous CUDA copy retires. */
  void before_write(unsigned frame,GLuint texture) {
    if(frame){GLenum layout=GL_LAYOUT_GENERAL_EXT;wait(gl_semaphores[1],0,nullptr,1,&texture,&layout);require(glGetError()==GL_NO_ERROR,"GL producer reuse wait failed");}
  }
  /** @brief Submit source readiness to the encoder stream without a CPU completion wait. */
  void ready_for_cuda(GLuint texture,CUstream stream) {
    GLenum layout=GL_LAYOUT_GENERAL_EXT;signal(gl_semaphores[0],0,nullptr,1,&texture,&layout);glFlush();require(glGetError()==GL_NO_ERROR,"GL source ready signal failed");
    CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS params{};cu(cuWaitExternalSemaphoresAsync(&cuda_semaphores[0],&params,1,stream));
  }
  /** @brief Publish read retirement after the queued CUDA copy, before a future GL overwrite. */
  void copied(CUstream stream) {
    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS params{};cu(cuSignalExternalSemaphoresAsync(&cuda_semaphores[1],&params,1,stream));
  }
};
