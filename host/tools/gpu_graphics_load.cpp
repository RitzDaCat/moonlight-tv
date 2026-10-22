// SPDX-License-Identifier: GPL-3.0-only
/** @file gpu_graphics_load.cpp
 * @brief Bounded offscreen graphics interference, calibrated by GPU elapsed queries.
 * No window, desktop configuration, driver settings, or disk cache is changed.
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

static void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
static volatile std::sig_atomic_t interrupted=0;
/** @brief Own partial initialization and all offscreen GPU objects. */
struct Load {
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    EGLSurface surface=EGL_NO_SURFACE;
    GLuint program=0,texture=0,framebuffer=0,vao=0,query=0;
    unsigned iterations=64,frame=0;
    ~Load(){
        if(query)glDeleteQueries(1,&query);
        if(vao)glDeleteVertexArrays(1,&vao);
        if(framebuffer)glDeleteFramebuffers(1,&framebuffer);
        if(texture)glDeleteTextures(1,&texture);
        if(program)glDeleteProgram(program);
        if(display!=EGL_NO_DISPLAY){eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(surface!=EGL_NO_SURFACE)eglDestroySurface(display,surface);if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);eglTerminate(display);}
    }
    /** @brief Compile a shader, retaining diagnostics only on failure. */
    static GLuint shader(GLenum type,const char *source){
        GLuint id=glCreateShader(type);glShaderSource(id,1,&source,nullptr);glCompileShader(id);
        GLint good=0;glGetShaderiv(id,GL_COMPILE_STATUS,&good);
        if(!good){char log[1024];glGetShaderInfoLog(id,sizeof(log),nullptr,log);glDeleteShader(id);throw std::runtime_error(log);}
        return id;
    }
    void init(){
        auto devices_fn=reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
        auto platform_fn=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        require(devices_fn&&platform_fn,"EGL device API unavailable");
        EGLDeviceEXT devices[16];EGLint count=0;require(devices_fn(16,devices,&count)&&count>0,"No EGL device");
        display=platform_fn(EGL_PLATFORM_DEVICE_EXT,devices[0],nullptr);require(display!=EGL_NO_DISPLAY&&eglInitialize(display,nullptr,nullptr),"EGL initialization failed");require(eglBindAPI(EGL_OPENGL_API),"OpenGL unavailable");
        EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};EGLConfig config;EGLint configs=0;require(eglChooseConfig(display,ca,&config,1,&configs)&&configs,"No EGL config");
        EGLint ctx[]={EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,5,EGL_NONE};context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);require(context!=EGL_NO_CONTEXT,"EGL context failed");
        EGLint sa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};surface=eglCreatePbufferSurface(display,config,sa);require(surface!=EGL_NO_SURFACE&&eglMakeCurrent(display,surface,surface,context),"EGL current context failed");
        const char *vendor=reinterpret_cast<const char*>(glGetString(GL_VENDOR));require(vendor&&std::strstr(vendor,"NVIDIA"),"NVIDIA OpenGL required");
        const char *vs="#version 450 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}";
        const char *fs="#version 450 core\nlayout(location=0) out vec4 color;uniform int iterations;uniform float seed;void main(){vec3 p=vec3(gl_FragCoord.xy/512.0,seed);for(int i=0;i<iterations;i++)p=fract(sin(p.yzx*vec3(13.2,27.1,7.8)+p.zxy+vec3(.2))*43758.5453);color=vec4(p,1);}";
        GLuint vertex=shader(GL_VERTEX_SHADER,vs),fragment=0;
        try{fragment=shader(GL_FRAGMENT_SHADER,fs);}catch(...){glDeleteShader(vertex);throw;}
        program=glCreateProgram();glAttachShader(program,vertex);glAttachShader(program,fragment);glLinkProgram(program);glDeleteShader(vertex);glDeleteShader(fragment);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);require(linked,"Shader program link failed");
        glUseProgram(program);glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,512,512);glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Incomplete framebuffer");
        glViewport(0,0,512,512);glGenVertexArrays(1,&vao);glBindVertexArray(vao);glGenQueries(1,&query);require(glGetError()==GL_NO_ERROR,"Graphics load initialization failed");
    }
    /** @brief Finish one draw, bounding outstanding work to one submission. */
    double draw(){
        glUniform1i(glGetUniformLocation(program,"iterations"),iterations);glUniform1f(glGetUniformLocation(program,"seed"),(++frame%256)/256.f);
        glBeginQuery(GL_TIME_ELAPSED,query);glDrawArrays(GL_TRIANGLES,0,3);glEndQuery(GL_TIME_ELAPSED);glFinish();
        GLuint64 ns=0;glGetQueryObjectui64v(query,GL_QUERY_RESULT,&ns);require(glGetError()==GL_NO_ERROR,"Graphics draw/query failed");return ns/1e6;
    }
    void run(unsigned seconds,double target,double period){
        double ms=0;
        for(unsigned step=0;step<8;++step){ms=draw();if(ms>=target*.8&&ms<=target*1.2)break;iterations=std::clamp(unsigned(iterations*target/std::max(ms,.001)),1u,32768u);}
        ms=draw();std::printf("{\"ready\":true,\"kind\":\"offscreen OpenGL fragment load\",\"iterations\":%u,\"calibrated_gpu_ms\":%.3f,\"target_gpu_ms\":%.3f,\"period_ms\":%.3f}\n",iterations,ms,target,period);std::fflush(stdout);
        auto start=std::chrono::steady_clock::now(),next=start,deadline=start+std::chrono::seconds(seconds);unsigned count=0,missed=0;double sum=0,maximum=0;
        while(!interrupted&&std::chrono::steady_clock::now()<deadline){double value=draw();sum+=value;maximum=std::max(maximum,value);++count;
          if(period>0){next+=std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double,std::milli>(period));auto now=std::chrono::steady_clock::now();if(next<now){++missed;next=now;}std::this_thread::sleep_until(next);}
        }
        std::printf("{\"draws\":%u,\"mean_gpu_ms\":%.3f,\"max_gpu_ms\":%.3f,\"elapsed_seconds\":%.3f,\"missed_periods\":%u}\n",count,sum/std::max(count,1u),maximum,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(),missed);
    }
};
int main(int argc,char **argv){try{require(argc>=2&&argc<=4,"Usage: gpu_graphics_load SECONDS (1..180) [DRAW_MS (.05..10) [PERIOD_MS (0 or DRAW_MS..100)]]");char *end=nullptr;long duration=std::strtol(argv[1],&end,10);require(end&&!*end&&duration>=1&&duration<=180,"Invalid bounded duration");double target=4;if(argc>=3){target=std::strtod(argv[2],&end);require(end&&!*end&&target>=.05&&target<=10,"Invalid draw target");}double period=0;if(argc==4){period=std::strtod(argv[3],&end);require(end&&!*end&&(period==0||(period>=target&&period<=100)),"Invalid graphics period");}std::signal(SIGTERM,[](int){interrupted=1;});Load load;load.init();load.run(unsigned(duration),target,period);return 0;}catch(const std::exception &error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
