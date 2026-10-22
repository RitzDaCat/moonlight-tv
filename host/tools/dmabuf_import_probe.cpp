// SPDX-License-Identifier: GPL-3.0-only
/** @file dmabuf_import_probe.cpp
 * @brief Real DMA-BUF import churn versus retained views, with changing-pixel checks.
 * Offscreen GBM allocations only; no game capture or display configuration changes.
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <gbm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include "src/platform/linux/dmabuf_import_cache.h"
/** @brief Reject unsupported operations rather than emit misleading timings. */
static void check(bool ok,const char *why) { if(!ok)throw std::runtime_error(why); }
/** @brief Scoped EGL view. Destroy GL texture before its backing EGL image. */
struct View {
 EGLDisplay display=EGL_NO_DISPLAY; EGLImageKHR image=EGL_NO_IMAGE_KHR; GLuint texture=0;
 View()=default;View(const View&)=delete;View& operator=(const View&)=delete;
 View(View&&b)noexcept:display(b.display),image(std::exchange(b.image,EGL_NO_IMAGE_KHR)),texture(std::exchange(b.texture,0)){}
 View& operator=(View&&b)noexcept{if(this!=&b){reset();display=b.display;image=std::exchange(b.image,EGL_NO_IMAGE_KHR);texture=std::exchange(b.texture,0);}return *this;}
 void reset() { if(texture)glDeleteTextures(1,&texture);texture=0;if(image!=EGL_NO_IMAGE_KHR)eglDestroyImage(display,image);image=EGL_NO_IMAGE_KHR; }
 ~View(){reset();}
};
/** @brief Own one GBM allocation, exported descriptors and two independently imported views. */
struct Buffer {
 gbm_bo *bo=nullptr; std::array<int,4> fds{-1,-1,-1,-1}; std::array<struct stat,4> identity{}; unsigned planes=0;
 View producer;
 ~Buffer(){producer.reset();for(int fd:fds)if(fd>=0)close(fd);if(bo)gbm_bo_destroy(bo);}
};
/** @brief Own GPU display context and render-node resources, including failed initialization. */
struct Environment {
 int fd=-1;gbm_device *gbm=nullptr;EGLDisplay display=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;EGLSurface surface=EGL_NO_SURFACE;GLuint fbo=0;
 ~Environment(){if(fbo)glDeleteFramebuffers(1,&fbo);if(display!=EGL_NO_DISPLAY){eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(surface!=EGL_NO_SURFACE)eglDestroySurface(display,surface);if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);eglTerminate(display);}if(gbm)gbm_device_destroy(gbm);if(fd>=0)close(fd);}
 /** @brief Open a render node and a matching offscreen EGL display. */
 void init(){
  fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);check(fd>=0,"Render node unavailable");gbm=gbm_create_device(fd);check(gbm,"GBM device unavailable");
  auto platform=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));check(platform,"EGL platform API unavailable");
  display=platform(EGL_PLATFORM_GBM_KHR,gbm,nullptr);check(display!=EGL_NO_DISPLAY&&eglInitialize(display,nullptr,nullptr),"GBM EGL initialize failed");
  check(eglBindAPI(EGL_OPENGL_API),"OpenGL unavailable");EGLint attrs[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};EGLConfig config;EGLint n=0;
  check(eglChooseConfig(display,attrs,&config,1,&n)&&n,"EGL config unavailable");EGLint ca[]={EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,5,EGL_NONE};context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);check(context!=EGL_NO_CONTEXT,"GL context unavailable");
  check(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context),"Surfaceless context unavailable");glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
 }
 /** @brief Import all plane metadata including the actual DRM format modifier. */
 void import(View &v,Buffer &b,unsigned width,unsigned height,const dma_import::surface *retained=nullptr){
  v.display=display;std::vector<EGLAttrib> a{EGL_WIDTH,width,EGL_HEIGHT,height,EGL_LINUX_DRM_FOURCC_EXT,gbm_bo_get_format(b.bo)};
  const EGLAttrib fd_attr[]={EGL_DMA_BUF_PLANE0_FD_EXT,EGL_DMA_BUF_PLANE1_FD_EXT,EGL_DMA_BUF_PLANE2_FD_EXT,EGL_DMA_BUF_PLANE3_FD_EXT};
  const EGLAttrib offset_attr[]={EGL_DMA_BUF_PLANE0_OFFSET_EXT,EGL_DMA_BUF_PLANE1_OFFSET_EXT,EGL_DMA_BUF_PLANE2_OFFSET_EXT,EGL_DMA_BUF_PLANE3_OFFSET_EXT};
  const EGLAttrib pitch_attr[]={EGL_DMA_BUF_PLANE0_PITCH_EXT,EGL_DMA_BUF_PLANE1_PITCH_EXT,EGL_DMA_BUF_PLANE2_PITCH_EXT,EGL_DMA_BUF_PLANE3_PITCH_EXT};
  const EGLAttrib lo_attr[]={EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
  const EGLAttrib hi_attr[]={EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
  auto modifier=gbm_bo_get_modifier(b.bo);
  for(unsigned p=0;p<b.planes;++p){a.insert(a.end(),{fd_attr[p],retained ? retained->fds[p] : b.fds[p],offset_attr[p],gbm_bo_get_offset(b.bo,p),pitch_attr[p],gbm_bo_get_stride_for_plane(b.bo,p)});if(modifier!=UINT64_MAX)a.insert(a.end(),{lo_attr[p],EGLAttrib(uint32_t(modifier)),hi_attr[p],EGLAttrib(uint32_t(modifier>>32))});}
  a.push_back(EGL_NONE);v.image=eglCreateImage(display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,a.data());check(v.image!=EGL_NO_IMAGE_KHR,"DMA-BUF EGL import failed");
  glGenTextures(1,&v.texture);glBindTexture(GL_TEXTURE_2D,v.texture);auto bind=reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));check(bind,"GL EGL-image API unavailable");bind(GL_TEXTURE_2D,v.image);glBindTexture(GL_TEXTURE_2D,0);check(glGetError()==GL_NO_ERROR,"DMA-BUF texture import failed");
 }
};
/** @brief Measure real imports while validating identity and changing source contents. */
int main(int argc,char**argv){try{
 unsigned samples=argc==2&&std::string(argv[1])=="--short"?100:200;check(argc==1||samples==100,"Usage: dmabuf_import_probe [--short]");
 Environment e;e.init();
 for(unsigned width:{1920u,3840u}){unsigned height=width==1920?1080:2160;std::array<Buffer,3> pool;
  for(auto &b:pool){b.bo=gbm_bo_create(e.gbm,width,height,GBM_FORMAT_XRGB8888,GBM_BO_USE_RENDERING);check(b.bo,"GBM allocation failed");b.planes=gbm_bo_get_plane_count(b.bo);check(b.planes>0&&b.planes<=4,"Invalid plane count");for(unsigned p=0;p<b.planes;++p){b.fds[p]=gbm_bo_get_fd_for_plane(b.bo,p);check(b.fds[p]>=0&&fstat(b.fds[p],&b.identity[p])==0,"Plane export failed");}e.import(b.producer,b,width,height);}
  for(unsigned round=0;round<2;++round)for(unsigned step=0;step<2;++step){bool cached=round?step==0:step==1;View transient;dma_import::cache<View> imports;std::vector<double> setup,complete;unsigned creations=0;
   for(unsigned frame=0;frame<samples+20;++frame){auto &b=pool[frame%pool.size()];unsigned char expected=static_cast<unsigned char>((frame*13)%256);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,b.producer.texture,0);check(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Producer framebuffer incomplete");glViewport(0,0,width,height);glClearColor(expected/255.f,0.25f,0.75f,1.f);glClear(GL_COLOR_BUFFER_BIT);glFinish();
    auto begin=std::chrono::steady_clock::now();dma_import::surface description;description.width=width;description.height=height;description.fourcc=gbm_bo_get_format(b.bo);description.modifier=gbm_bo_get_modifier(b.bo);dma_import::owned_fds exported;bool same=true;for(unsigned p=0;p<b.planes;++p){int fd=gbm_bo_get_fd_for_plane(b.bo,p);exported.values[p]=description.fds[p]=fd;description.pitches[p]=gbm_bo_get_stride_for_plane(b.bo,p);description.offsets[p]=gbm_bo_get_offset(b.bo,p);struct stat current{};check(fd>=0&&fstat(fd,&current)==0,"Repeated export failed");same&=current.st_dev==b.identity[p].st_dev&&current.st_ino==b.identity[p].st_ino;}check(same,"Re-exported allocation identity changed; cache key invalid");
    GLuint texture;if(cached){auto view=imports.get(description,[&](const dma_import::surface &retained)->std::optional<View>{++creations;View result;e.import(result,b,width,height,&retained);return result;});check(view,"Cached import failed");texture=view->texture;}else{transient.reset();e.import(transient,b,width,height,&description);texture=transient.texture;}
    auto ready=std::chrono::steady_clock::now();glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);check(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Consumer framebuffer incomplete");unsigned char pixel[4]{};glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);check(glGetError()==GL_NO_ERROR&&std::abs(int(pixel[0])-expected)<=1,"Retained view did not see updated pixels");auto done=std::chrono::steady_clock::now();
    if(frame>=20){setup.push_back(std::chrono::duration<double,std::micro>(ready-begin).count());complete.push_back(std::chrono::duration<double,std::micro>(done-begin).count());}
   }
   auto stats=[](std::vector<double> v){std::sort(v.begin(),v.end());char b[160];std::snprintf(b,sizeof(b),"{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}",v[v.size()/2],v[v.size()*95/100],v[v.size()*99/100]);return std::string(b);};
   std::printf("{\"width\":%u,\"height\":%u,\"round\":%u,\"cached\":%s,\"samples\":%u,\"all_identity_checks_passed\":true,\"updated_pixel_checks\":%u,\"hits\":%u,\"modifier\":%llu,\"setup_us\":%s,\"setup_and_consumer_us\":%s}\n",width,height,round,cached?"true":"false",samples,samples+20,cached?samples+20-creations:0,(unsigned long long)gbm_bo_get_modifier(pool[0].bo),stats(setup).c_str(),stats(complete).c_str());std::fflush(stdout);
  }
 }
 return 0;
}catch(const std::exception &error){std::fprintf(stderr,"%s EGL=0x%x GL=0x%x\n",error.what(),eglGetError(),glGetError());return 1;}}
