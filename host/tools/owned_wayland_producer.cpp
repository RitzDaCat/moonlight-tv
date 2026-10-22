// SPDX-License-Identifier: GPL-3.0-only
/** Normal, owned Wayland client with a visible frame-ID witness; never targets another app. */
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include "xdg-shell-client.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef OWNED_REFERENCE_CAPTURE
#include <png.h>
#include <filesystem>
#include <vector>
#endif
static volatile std::sig_atomic_t stopped=0;
static void require(bool yes,const char *label){if(!yes)throw std::runtime_error(label);}
using Clock=std::chrono::steady_clock;
struct Producer {
  wl_display *display=nullptr;wl_registry *registry=nullptr;wl_compositor *compositor=nullptr;wl_output *output=nullptr;
  xdg_wm_base *shell=nullptr;wl_surface *surface=nullptr;xdg_surface *role=nullptr;xdg_toplevel *toplevel=nullptr;wl_egl_window *window=nullptr;
  EGLDisplay egl=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;EGLSurface target=EGL_NO_SURFACE;GLuint program=0,vao=0;
  bool configured=false;int width=3840,height=2160;
#ifdef OWNED_REFERENCE_CAPTURE
  std::filesystem::path references;
  std::vector<unsigned char> reference_pixels;
  unsigned reference_count=0;
  /** @brief Save the exact owned GLES backbuffer with conventional top-down PNG orientation. */
  void write_reference(unsigned frame){
    png_image image{};image.version=PNG_IMAGE_VERSION;image.width=width;image.height=height;image.format=PNG_FORMAT_RGBA;
    auto file=references/(std::to_string(frame)+".png");
    require(png_image_write_to_file(&image,file.c_str(),0,reference_pixels.data(),-width*4,nullptr)!=0,"Owned source PNG write failed");
    png_image_free(&image);++reference_count;
    std::printf("{\"reference_frame\":%u,\"width\":%d,\"height\":%d}\n",frame,width,height);std::fflush(stdout);
  }
#endif
  static void output_geometry(void *,wl_output *,int32_t,int32_t,int32_t,int32_t,int32_t,const char *,const char *,int32_t){}
  static void output_mode(void *,wl_output *,uint32_t,int32_t,int32_t,int32_t){}
  ~Producer(){if(egl!=EGL_NO_DISPLAY){if(program)glDeleteProgram(program);if(vao)glDeleteVertexArrays(1,&vao);eglMakeCurrent(egl,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(target!=EGL_NO_SURFACE)eglDestroySurface(egl,target);if(context!=EGL_NO_CONTEXT)eglDestroyContext(egl,context);eglTerminate(egl);}if(window)wl_egl_window_destroy(window);if(toplevel)xdg_toplevel_destroy(toplevel);if(role)xdg_surface_destroy(role);if(surface)wl_surface_destroy(surface);if(shell)xdg_wm_base_destroy(shell);if(output)wl_output_destroy(output);if(compositor)wl_compositor_destroy(compositor);if(registry)wl_registry_destroy(registry);if(display)wl_display_disconnect(display);}
  static void global(void *data,wl_registry *r,uint32_t name,const char *interface,uint32_t version){auto &p=*static_cast<Producer*>(data);if(!std::strcmp(interface,wl_compositor_interface.name))p.compositor=static_cast<wl_compositor*>(wl_registry_bind(r,name,&wl_compositor_interface,std::min(version,4u)));else if(!std::strcmp(interface,xdg_wm_base_interface.name))p.shell=static_cast<xdg_wm_base*>(wl_registry_bind(r,name,&xdg_wm_base_interface,std::min(version,6u)));else if(!p.output&&!std::strcmp(interface,wl_output_interface.name)){p.output=static_cast<wl_output*>(wl_registry_bind(r,name,&wl_output_interface,1));static const wl_output_listener listener{output_geometry,output_mode};wl_output_add_listener(p.output,&listener,&p);}}
  static void removed(void *,wl_registry *,uint32_t){}
  static void ping(void *,xdg_wm_base *shell,uint32_t serial){xdg_wm_base_pong(shell,serial);}
  static void configure(void *data,xdg_surface *surface,uint32_t serial){auto &p=*static_cast<Producer*>(data);xdg_surface_ack_configure(surface,serial);p.configured=true;}
  static void size(void *data,xdg_toplevel *,int32_t w,int32_t h,wl_array *){auto &p=*static_cast<Producer*>(data);if(w>0&&h>0){p.width=w;p.height=h;if(p.window)wl_egl_window_resize(p.window,w,h,0,0);}}
  static void close(void *,xdg_toplevel *){stopped=1;}
  static void bounds(void *,xdg_toplevel *,int32_t,int32_t){}
  static void capabilities(void *,xdg_toplevel *,wl_array *){}
  static GLuint shader(GLenum type,const char *source){GLuint s=glCreateShader(type);glShaderSource(s,1,&source,nullptr);glCompileShader(s);GLint valid=0;glGetShaderiv(s,GL_COMPILE_STATUS,&valid);if(!valid){char text[1024];glGetShaderInfoLog(s,sizeof(text),nullptr,text);glDeleteShader(s);throw std::runtime_error(text);}return s;}
  void init(int w,int h){
    width=w;height=h;display=wl_display_connect(nullptr);require(display,"Owned Wayland display unavailable");registry=wl_display_get_registry(display);static const wl_registry_listener registry_listener{global,removed};wl_registry_add_listener(registry,&registry_listener,this);require(wl_display_roundtrip(display)>=0&&compositor&&shell&&output,"Required Wayland globals missing");static const xdg_wm_base_listener shell_listener{ping};xdg_wm_base_add_listener(shell,&shell_listener,this);
    surface=wl_compositor_create_surface(compositor);role=xdg_wm_base_get_xdg_surface(shell,surface);static const xdg_surface_listener role_listener{configure};xdg_surface_add_listener(role,&role_listener,this);toplevel=xdg_surface_get_toplevel(role);static const xdg_toplevel_listener top_listener{size,close,bounds,capabilities};xdg_toplevel_add_listener(toplevel,&top_listener,this);xdg_toplevel_set_app_id(toplevel,"org.moonlight.owned-streaming-producer");xdg_toplevel_set_title(toplevel,"Owned streaming latency producer");xdg_toplevel_set_fullscreen(toplevel,output);wl_surface_commit(surface);while(!configured&&!stopped)require(wl_display_dispatch(display)>=0,"Wayland configure failed");require(!stopped,"Producer closed before configure");
    egl=eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT,display,nullptr);require(egl!=EGL_NO_DISPLAY&&eglInitialize(egl,nullptr,nullptr),"Wayland EGL unavailable");require(eglBindAPI(EGL_OPENGL_ES_API),"GLES unavailable");EGLint attributes[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};EGLConfig config;EGLint count=0;require(eglChooseConfig(egl,attributes,&config,1,&count)&&count,"No GLES window configuration");EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};context=eglCreateContext(egl,config,EGL_NO_CONTEXT,ctx);require(context!=EGL_NO_CONTEXT,"GLES context failed");window=wl_egl_window_create(surface,width,height);require(window,"Wayland EGL window failed");target=eglCreateWindowSurface(egl,config,reinterpret_cast<EGLNativeWindowType>(window),nullptr);require(target!=EGL_NO_SURFACE&&eglMakeCurrent(egl,target,target,context),"GLES current context failed");eglSwapInterval(egl,0);
    const char *vendor=reinterpret_cast<const char*>(glGetString(GL_VENDOR)),*renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));require(vendor&&std::strstr(vendor,"NVIDIA"),"Owned producer is not on the intended NVIDIA GPU");
    const char *vs="#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}";
    const char *fs=R"glsl(#version 300 es
precision highp float;
precision highp int;
uniform uint frame;
uniform ivec2 canvas;
out vec4 color;
void main() {
    vec2 p=gl_FragCoord.xy;
    bool strip=p.y<16.0 || (p.y>=float(canvas.y/2) && p.y<float(canvas.y/2+16)) || p.y>=float(canvas.y-16);
    bool right=p.x>=float(canvas.x-512);
    if (strip && (p.x<512.0 || right)) {
        float x=p.x-(right ? float(canvas.x-512) : 0.0);
        uint bit=(frame>>uint(x/16.0))&1u;
        color=vec4(vec3(float(bit)),1);
        return;
    }
    float x=p.x-float(frame)*4.0;
    float y=p.y;
    float v=.5+.24*sin(x/23.0)+.12*cos(y/51.0);
    color=vec4(v,.3+.2*sin(y/70.0),.4+.2*cos(x/90.0),1);
}
)glsl";
    GLuint vertex=shader(GL_VERTEX_SHADER,vs),fragment=0;try{fragment=shader(GL_FRAGMENT_SHADER,fs);}catch(...){glDeleteShader(vertex);throw;}program=glCreateProgram();glAttachShader(program,vertex);glAttachShader(program,fragment);glLinkProgram(program);glDeleteShader(vertex);glDeleteShader(fragment);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);require(linked,"Producer program link failed");glUseProgram(program);glGenVertexArrays(1,&vao);glBindVertexArray(vao);require(glGetError()==GL_NO_ERROR,"Producer initialization failed");std::printf("{\"ready\":true,\"vendor\":\"%s\",\"renderer\":\"%s\",\"width\":%d,\"height\":%d,\"witness_regions\":6}\n",vendor,renderer,width,height);std::fflush(stdout);
  }
  void run(unsigned seconds,unsigned fps){auto start=Clock::now(),end=start+std::chrono::seconds(seconds);unsigned frame=0;GLint location=glGetUniformLocation(program,"frame");GLint canvas=glGetUniformLocation(program,"canvas");
    while(!stopped&&Clock::now()<end){std::this_thread::sleep_until(start+std::chrono::nanoseconds(uint64_t(frame)*1000000000/fps));
      while(wl_display_prepare_read(display)!=0)require(wl_display_dispatch_pending(display)>=0,"Wayland dispatch failed");wl_display_flush(display);pollfd fd{wl_display_get_fd(display),POLLIN,0};if(poll(&fd,1,0)>0)require(wl_display_read_events(display)>=0,"Wayland read failed");else wl_display_cancel_read(display);require(wl_display_dispatch_pending(display)>=0,"Wayland dispatch failed");
      auto begin=Clock::now();++frame;glViewport(0,0,width,height);glUniform1ui(location,frame);glUniform2i(canvas,width,height);glDrawArrays(GL_TRIANGLES,0,3);
#ifdef OWNED_REFERENCE_CAPTURE
      bool save_reference=!references.empty() && frame%60==0 && reference_count<128;
      if(save_reference){reference_pixels.resize(size_t(width)*height*4);glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,reference_pixels.data());}
#endif
      require(glGetError()==GL_NO_ERROR&&eglSwapBuffers(egl,target),"Owned source draw/swap failed");auto done=Clock::now();auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count();std::printf("{\"frame\":%u,\"begin_ns\":%lld,\"swap_cpu_us\":%.3f}\n",frame,static_cast<long long>(ns),std::chrono::duration<double,std::micro>(done-begin).count());std::fflush(stdout);
#ifdef OWNED_REFERENCE_CAPTURE
      if(save_reference)write_reference(frame);
#endif
    }
  }
};
int main(int argc,char **argv){try{require(argc==5||argc==6,"Usage: owned-wayland-producer WIDTH HEIGHT FPS SECONDS [OWNED_REFERENCE_DIRECTORY]");auto number=[](const char *s,unsigned lo,unsigned hi){std::string v=s;size_t used=0;auto n=std::stoul(v,&used);require(used==v.size()&&n>=lo&&n<=hi,"Producer argument out of bounds");return unsigned(n);};unsigned w=number(argv[1],512,3840),h=number(argv[2],16,2160),fps=number(argv[3],30,240),seconds=number(argv[4],1,1200);require(w%2==0&&h%2==0,"Producer geometry must be even");std::signal(SIGTERM,[](int){stopped=1;});Producer p;
#ifdef OWNED_REFERENCE_CAPTURE
if(argc==6){p.references=argv[5];require(std::filesystem::is_directory(p.references),"Owned reference directory missing");}
#else
require(argc==5,"Reference capture was not compiled into this producer");
#endif
p.init(w,h);p.run(seconds,fps);return 0;}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
