// SPDX-License-Identifier: GPL-3.0-only
/** @file kms_metadata_probe.cpp
 * @brief Rootless, read-only DRM metadata timing. Never modesets or modifies a framebuffer.
 */
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
/** @brief Own just this probe's opened DRM descriptor. */
struct Card {int fd=-1;~Card(){if(fd>=0)close(fd);}};
/** @brief Own libdrm's property response. */
struct Props {void operator()(drmModeObjectProperties*p)const{drmModeFreeObjectProperties(p);}};
/** @brief Own libdrm's plane response. */
struct Plane {void operator()(drmModePlane*p)const{drmModeFreePlane(p);}};
/** @brief Reject errors instead of counting failed operations as fast samples. */
static void check(bool ok,const char*label){if(!ok)throw std::runtime_error(label);}
/** @brief Query current values, optionally refetching every immutable descriptor as Sunshine does. */
static unsigned props(int fd,unsigned object,unsigned type,bool descriptors){
 std::unique_ptr<drmModeObjectProperties,Props> p(drmModeObjectGetProperties(fd,object,type));check(bool(p),"Object property query failed");
 if(descriptors)for(unsigned i=0;i<p->count_props;++i){auto prop=drmModeGetProperty(fd,p->props[i]);check(prop,"Property descriptor query failed");drmModeFreeProperty(prop);}
 return p->count_props;
}
/** @brief Time short sequences with order reversal and no pacing or presentation changes. */
template<class F> static void measure(const char*stage,unsigned object,unsigned round,F&&f){
 std::vector<double> times;for(unsigned i=0;i<120;++i){auto a=std::chrono::steady_clock::now();f();auto b=std::chrono::steady_clock::now();if(i>=20)times.push_back(std::chrono::duration<double,std::micro>(b-a).count());}
 std::sort(times.begin(),times.end());std::printf("{\"stage\":\"%s\",\"object\":%u,\"round\":%u,\"samples\":100,\"us\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}}\n",stage,object,round,times[50],times[95],times[99]);
}
/** @brief Inspect active planes/connectors with rootless access and no capture-resource export. */
int main(int argc,char**argv){try{
 check(argc<=2,"Usage: kms_metadata_probe [/dev/dri/cardN]");Card card;card.fd=open(argc==2?argv[1]:"/dev/dri/card1",O_RDWR|O_CLOEXEC);check(card.fd>=0,"DRM node unavailable");
 check(!drmSetClientCap(card.fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1),"Universal plane enumeration unavailable");
 check(!drmSetClientCap(card.fd,DRM_CLIENT_CAP_ATOMIC,1),"Atomic property metadata unavailable");
 auto planes=std::unique_ptr<drmModePlaneRes,decltype(&drmModeFreePlaneResources)>(drmModeGetPlaneResources(card.fd),drmModeFreePlaneResources);check(bool(planes),"Plane enumeration failed");
 for(unsigned i=0;i<planes->count_planes;++i){unsigned id=planes->planes[i];std::unique_ptr<drmModePlane,Plane> plane(drmModeGetPlane(card.fd,id));check(bool(plane),"Plane query failed");
  std::unique_ptr<drmModeObjectProperties,Props> p(drmModeObjectGetProperties(card.fd,id,DRM_MODE_OBJECT_PLANE));check(bool(p),"Plane properties unavailable");unsigned kind=999;
  for(unsigned n=0;n<p->count_props;++n){auto prop=drmModeGetProperty(card.fd,p->props[n]);check(prop,"Property descriptor unavailable");if(!std::strcmp(prop->name,"type"))kind=p->prop_values[n];drmModeFreeProperty(prop);}
  std::printf("{\"plane\":%u,\"type\":%u,\"fb\":%u,\"crtc\":%u,\"property_count\":%u}\n",id,kind,plane->fb_id,plane->crtc_id,p->count_props);
  if(!plane->crtc_id&&kind!=DRM_PLANE_TYPE_CURSOR)continue;
  for(unsigned round=0;round<2;++round){
   measure("plane_query",id,round,[&](){std::unique_ptr<drmModePlane,Plane> v(drmModeGetPlane(card.fd,id));check(bool(v),"Repeated plane query failed");});
   for(unsigned step=0;step<2;++step){bool full=round?step==1:step==0;measure(full?"values_and_descriptors":"values_only",id,round,[&](){props(card.fd,id,DRM_MODE_OBJECT_PLANE,full);});}
  }
 }
 auto resources=std::unique_ptr<drmModeRes,decltype(&drmModeFreeResources)>(drmModeGetResources(card.fd),drmModeFreeResources);check(bool(resources),"DRM resources unavailable");
 for(int i=0;i<resources->count_connectors;++i){unsigned id=resources->connectors[i];for(unsigned round=0;round<2;++round)for(unsigned step=0;step<2;++step){bool full=round?step==1:step==0;measure(full?"connector_values_and_descriptors":"connector_values_only",id,round,[&](){props(card.fd,id,DRM_MODE_OBJECT_CONNECTOR,full);});}}
 return 0;
}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
