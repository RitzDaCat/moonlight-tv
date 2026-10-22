// SPDX-License-Identifier: GPL-3.0-only
/** Lossless causal residual/nibble transport prototype. No native-codec integration. */
#pragma once
#include <cub/cub.cuh>
#include <array>
#include <cstdint>
namespace residual {
using Clock=std::chrono::steady_clock;
static constexpr unsigned header_bytes=32;
static bool compress_modes=false;
enum Mode:unsigned char {Zero=0,Constant=1,Nibbles=2,Raw=3};
__device__ static int predicted(const unsigned char *reference,size_t pitch,int w,int h,size_t i,int dx,int dy){
  bool uv=i>=size_t(w)*h;size_t local=uv?i-size_t(w)*h:i;
  int x=int(local%w)-dx,y=int(local/w)-(uv?dy/2:dy);
  if(x<0||x>=w||y<0||y>=(uv?h/2:h))return -1;
  return reference[size_t(y+(uv?h:0))*pitch+x];
}
struct Limits{int lo,hi;};
struct Combine{__device__ Limits operator()(Limits a,Limits b)const{return {min(a.lo,b.lo),max(a.hi,b.hi)};}};
template<unsigned Tile>__global__ void classify(const unsigned char *source,size_t sp,const unsigned char *reference,size_t rp,int w,int h,int dx,int dy,unsigned char *modes,uint32_t *lengths){
  using Reduce=cub::BlockReduce<Limits,256>;__shared__ typename Reduce::TempStorage temporary;
  size_t start=size_t(blockIdx.x)*Tile,n=size_t(w)*h*3/2;Limits bounds{256,-256};
  for(unsigned offset=threadIdx.x;offset<Tile&&start+offset<n;offset+=blockDim.x){size_t i=start+offset;int prediction=predicted(reference,rp,w,h,i,dx,dy);int value=source[(i/w)*sp+i%w];if(prediction<0){bounds.lo=-256;bounds.hi=256;}else{int delta=value-prediction;bounds.lo=min(bounds.lo,delta);bounds.hi=max(bounds.hi,delta);}}
  auto aggregate=Reduce(temporary).Reduce(bounds,Combine{});
  if(!threadIdx.x){unsigned active=unsigned(min(size_t(Tile),n-start));Mode mode=aggregate.lo==0&&aggregate.hi==0?Zero:aggregate.lo==aggregate.hi&&aggregate.lo>=-8&&aggregate.hi<=7?Constant:aggregate.lo>=-8&&aggregate.hi<=7?Nibbles:Raw;modes[blockIdx.x]=mode;lengths[blockIdx.x]=mode==Zero?0:mode==Constant?1:mode==Nibbles?(active+1)/2:active;}
}
__global__ void pack_modes(const unsigned char *modes,unsigned char *packed,unsigned tiles){unsigned p=blockIdx.x*blockDim.x+threadIdx.x;if(p>=(tiles+3)/4)return;unsigned value=0;for(unsigned j=0;j<4&&p*4+j<tiles;++j)value|=unsigned(modes[p*4+j])<<(j*2);packed[p]=value;}
template<unsigned Tile>__global__ void pack(const unsigned char *source,size_t sp,const unsigned char *reference,size_t rp,int w,int h,int dx,int dy,const unsigned char *modes,const uint32_t *offsets,unsigned char *payload){
  unsigned mode=modes[blockIdx.x];size_t start=size_t(blockIdx.x)*Tile,n=size_t(w)*h*3/2;unsigned active=unsigned(min(size_t(Tile),n-start));auto dst=payload+offsets[blockIdx.x];
  if(mode==Zero)return;
  if(mode==Constant){if(!threadIdx.x){int delta=source[(start/w)*sp+start%w]-predicted(reference,rp,w,h,start,dx,dy);dst[0]=delta+8;}return;}
  if(mode==Raw){for(unsigned o=threadIdx.x;o<active;o+=blockDim.x){size_t i=start+o;dst[o]=source[(i/w)*sp+i%w];}return;}
  for(unsigned pair=threadIdx.x;pair<(active+1)/2;pair+=blockDim.x){unsigned value=0;for(unsigned j=0;j<2&&pair*2+j<active;++j){size_t i=start+pair*2+j;int delta=source[(i/w)*sp+i%w]-predicted(reference,rp,w,h,i,dx,dy);value|=unsigned(delta+8)<<(j*4);}dst[pair]=value;}
}
template<unsigned Tile>__global__ void receive_modes(const unsigned char *wire,uint32_t *lengths,unsigned tiles,size_t n){unsigned tile=blockIdx.x*blockDim.x+threadIdx.x;if(tile>=tiles)return;unsigned mode=(wire[tile/4]>>(2*(tile%4)))&3,active=unsigned(min(size_t(Tile),n-size_t(tile)*Tile));lengths[tile]=mode==Zero?0:mode==Constant?1:mode==Nibbles?(active+1)/2:active;}
template<unsigned Tile>__global__ void restore(const unsigned char *wire,const unsigned char *payload,unsigned payload_bytes,const uint32_t *offsets,const unsigned char *reference,size_t rp,unsigned char *out,size_t op,int w,int h,int dx,int dy,unsigned *error){
  unsigned tile=blockIdx.x,mode=(wire[tile/4]>>(2*(tile%4)))&3;size_t start=size_t(tile)*Tile,n=size_t(w)*h*3/2;unsigned active=unsigned(min(size_t(Tile),n-start));unsigned offset=offsets[tile],length=offsets[tile+1]-offset;
  if(offset>payload_bytes||length>payload_bytes-offset){if(!threadIdx.x)atomicOr(error,1u);return;}
  if(tile==gridDim.x-1&&offset+length!=payload_bytes){if(!threadIdx.x)atomicOr(error,2u);return;}
  if(mode==Constant&&payload[offset]>15){if(!threadIdx.x)atomicOr(error,4u);return;}
  for(unsigned o=threadIdx.x;o<active;o+=blockDim.x){size_t i=start+o;int value;
    if(mode==Raw)value=payload[offset+o];
    else{int prediction=predicted(reference,rp,w,h,i,dx,dy);if(prediction<0){atomicOr(error,8u);continue;}int delta=mode==Zero?0:mode==Constant?int(payload[offset])-8:int((payload[offset+o/2]>>(4*(o%2)))&15)-8;value=prediction+delta;if(value<0||value>255){atomicOr(error,16u);continue;}}
    out[(i/w)*op+i%w]=value;
  }
}
static void put(std::vector<unsigned char>&v,unsigned p,uint32_t x,unsigned bytes){for(unsigned i=0;i<bytes;++i)v[p+i]=x>>(i*8);}
static uint32_t get(const std::vector<unsigned char>&v,unsigned p,unsigned bytes){require(p+bytes<=v.size(),"Short residual header");uint32_t x=0;for(unsigned i=0;i<bytes;++i)x|=uint32_t(v[p+i])<<(i*8);return x;}
// Independent scalar addressing/decoder. Only the received modes/payload are authoritative.
static int cpu_prediction(const std::vector<unsigned char>&r,int w,int h,size_t pixel,int dx,int dy){size_t row=pixel/w,column=pixel%w;bool chroma=row>=unsigned(h);int ry=int(row)-(chroma?dy/2:dy),rx=int(column)-dx;if(rx<0||rx>=w||ry<(chroma?h:0)||ry>=(chroma?h*3/2:h))return -1;return r[size_t(ry)*w+rx];}
static std::vector<unsigned char> cpu_decode(const std::vector<unsigned char>&wire,const std::vector<unsigned char>&reference,unsigned expected_ref){
  require(wire.size()>=header_bytes&&get(wire,0,4)==0x31424e52&&(get(wire,22,2)==1||get(wire,22,2)==2),"Bad residual header/version");
  unsigned w=get(wire,4,2),h=get(wire,6,2),tile=get(wire,20,2),modes=get(wire,24,4),payload=get(wire,28,4);int dx=int16_t(get(wire,16,2)),dy=int16_t(get(wire,18,2));
  require((tile==128||tile==512||tile==2048)&&w&&h&&w<=8192&&h<=8192&&w%2==0&&h%2==0&&dx%2==0&&dy%2==0,"Invalid residual geometry/motion");
  size_t n=size_t(w)*h*3/2,tiles=(n+tile-1)/tile;
  require(get(wire,12,4)==expected_ref&&reference.size()==n,"Missing or wrong residual reference");require(wire.size()==header_bytes+modes+payload,"Invalid residual packet length");
  std::vector<unsigned char> flags;
  if(get(wire,22,2)==1){require(modes==(tiles+3)/4,"Wrong literal mode length");flags.assign(wire.begin()+header_bytes,wire.begin()+header_bytes+modes);}
  else {require(modes%3==0,"Wrong compressed mode length");for(size_t p=header_bytes;p<header_bytes+modes;p+=3){unsigned count=wire[p]+256u*wire[p+1];require(count&&flags.size()+count<=(tiles+3)/4,"Invalid compressed mode run");flags.insert(flags.end(),count,wire[p+2]);}require(flags.size()==(tiles+3)/4,"Incomplete compressed mode runs");}
  if(tiles%4)require((flags.back()>>(2*(tiles%4)))==0,"Nonzero residual mode padding");
  std::vector<unsigned char> out(n);size_t cursor=header_bytes+modes;
  for(size_t t=0;t<tiles;++t){unsigned mode=(flags[t/4]>>(2*(t%4)))&3;size_t begin=t*tile,active=std::min(size_t(tile),n-begin),length=mode==Zero?0:mode==Constant?1:mode==Nibbles?(active+1)/2:active;
    require(cursor+length<=wire.size(),"Residual payload truncated");
    if(mode==Constant)require(wire[cursor]<=15,"Invalid constant residual");
    for(size_t j=0;j<active;++j){if(mode==Raw)out[begin+j]=wire[cursor+j];else{int p=cpu_prediction(reference,w,h,begin+j,dx,dy);require(p>=0,"Residual predictor outside reference");int delta=mode==Zero?0:mode==Constant?int(wire[cursor])-8:int((wire[cursor+j/2]>>(4*(j%2)))&15)-8;require(p+delta>=0&&p+delta<=255,"Residual arithmetic overflow");out[begin+j]=p+delta;}}
    cursor+=length;
  }
  require(cursor==wire.size(),"Trailing residual payload");return out;
}
template<unsigned Tile>struct Codec {
  int w,h;size_t n,tiles,mode_bytes;Image current,reference,decoded;Bytes modes,lengths,offsets,wire_modes,payload,receive_payload,receive_modes_buffer,decode_lengths,decode_offsets,error;
  Timer timer;void *scan_temp=nullptr;size_t scan_bytes=0;unsigned char *host=nullptr;uint32_t *host_size=nullptr;unsigned *host_error=nullptr;
  Codec(int w,int h):w(w),h(h),n(size_t(w)*h*3/2),tiles((n+Tile-1)/Tile),mode_bytes((tiles+3)/4),current(w,h),reference(w,h),decoded(w,h),modes(tiles),lengths((tiles+1)*4),offsets((tiles+1)*4),wire_modes(mode_bytes),payload(n),receive_payload(n),receive_modes_buffer(mode_bytes),decode_lengths((tiles+1)*4),decode_offsets((tiles+1)*4),error(4){
    try{check(cub::DeviceScan::ExclusiveSum(nullptr,scan_bytes,reinterpret_cast<uint32_t*>(lengths.data),reinterpret_cast<uint32_t*>(offsets.data),tiles+1,timer.stream));check(cudaMalloc(&scan_temp,scan_bytes));check(cudaMallocHost(&host,header_bytes+mode_bytes+n));check(cudaMallocHost(&host_size,4));check(cudaMallocHost(&host_error,4));}catch(...){clear();throw;}
  }
  void clear(){if(scan_temp)cudaFree(scan_temp);if(host)cudaFreeHost(host);if(host_size)cudaFreeHost(host_size);if(host_error)cudaFreeHost(host_error);}
  ~Codec(){clear();}
  std::vector<unsigned char> encode(int dx,int dy){
    require(dx%2==0&&dy%2==0,"Residual motion must preserve chroma pairs");
    auto s=timer.stream;classify<Tile><<<tiles,256,0,s>>>(current.data,current.pitch,reference.data,reference.pitch,w,h,dx,dy,modes.data,reinterpret_cast<uint32_t*>(lengths.data));
    check(cudaMemsetAsync(lengths.data+tiles*4,0,4,s));check(cub::DeviceScan::ExclusiveSum(scan_temp,scan_bytes,reinterpret_cast<uint32_t*>(lengths.data),reinterpret_cast<uint32_t*>(offsets.data),tiles+1,s));
    pack_modes<<<(mode_bytes+255)/256,256,0,s>>>(modes.data,wire_modes.data,tiles);
    pack<Tile><<<tiles,256,0,s>>>(current.data,current.pitch,reference.data,reference.pitch,w,h,dx,dy,modes.data,reinterpret_cast<uint32_t*>(offsets.data),payload.data);
    check(cudaGetLastError());check(cudaMemcpyAsync(host_size,offsets.data+tiles*4,4,cudaMemcpyDeviceToHost,s));check(cudaStreamSynchronize(s));require(*host_size<=n,"Residual output exceeds maximum");
    check(cudaMemcpyAsync(host+header_bytes,wire_modes.data,mode_bytes,cudaMemcpyDeviceToHost,s));if(*host_size)check(cudaMemcpyAsync(host+header_bytes+mode_bytes,payload.data,*host_size,cudaMemcpyDeviceToHost,s));check(cudaStreamSynchronize(s));
    std::vector<unsigned char> rle;if(compress_modes){for(size_t i=0;i<mode_bytes;){size_t j=i+1;while(j<mode_bytes&&j-i<65535&&host[header_bytes+j]==host[header_bytes+i])++j;rle.push_back((j-i)&255);rle.push_back((j-i)>>8);rle.push_back(host[header_bytes+i]);i=j;}}
    bool compressed=compress_modes&&rle.size()<mode_bytes;size_t wire_bytes=compressed?rle.size():mode_bytes;
    std::vector<unsigned char> packet(header_bytes+wire_bytes+*host_size);
    if(compressed)std::copy(rle.begin(),rle.end(),packet.begin()+header_bytes);else std::copy_n(host+header_bytes,mode_bytes,packet.begin()+header_bytes);
    std::copy_n(host+header_bytes+mode_bytes,*host_size,packet.begin()+header_bytes+wire_bytes);
    put(packet,0,0x31424e52,4);put(packet,4,w,2);put(packet,6,h,2);put(packet,8,1,4);put(packet,12,0,4);put(packet,16,uint16_t(dx),2);put(packet,18,uint16_t(dy),2);put(packet,20,Tile,2);put(packet,22,compressed?2:1,2);put(packet,24,wire_bytes,4);put(packet,28,*host_size,4);return packet;
  }
  void decode(const std::vector<unsigned char>&packet){
    require(packet.size()>=header_bytes&&get(packet,0,4)==0x31424e52&&(get(packet,22,2)==1||get(packet,22,2)==2),"Bad residual header");require(get(packet,4,2)==unsigned(w)&&get(packet,6,2)==unsigned(h)&&get(packet,20,2)==Tile&&get(packet,12,4)==0,"Residual reference/geometry mismatch");unsigned size=get(packet,28,4),wire_bytes=get(packet,24,4);int dx=int16_t(get(packet,16,2)),dy=int16_t(get(packet,18,2));require(dx%2==0&&dy%2==0&&size<=n&&packet.size()==header_bytes+wire_bytes+size,"Residual payload length/motion mismatch");
    if(get(packet,22,2)==1){require(wire_bytes==mode_bytes,"Wrong literal mode bytes");std::copy_n(packet.data()+header_bytes,mode_bytes,host+header_bytes);}
    else {require(wire_bytes%3==0,"Wrong mode run table");size_t count=0;for(size_t p=header_bytes;p<header_bytes+wire_bytes;p+=3){size_t run=uint16_t(packet[p])|(uint16_t(packet[p+1])<<8);require(run&&run<=mode_bytes-count,"Invalid mode run");std::fill_n(host+header_bytes+count,run,packet[p+2]);count+=run;}require(count==mode_bytes,"Incomplete mode run table");}
    if(tiles%4)require((host[header_bytes+mode_bytes-1]>>(2*(tiles%4)))==0,"Nonzero GPU mode padding");
    std::copy_n(packet.data()+header_bytes+wire_bytes,size,host+header_bytes+mode_bytes);auto s=timer.stream;
    check(cudaMemcpyAsync(receive_modes_buffer.data,host+header_bytes,mode_bytes,cudaMemcpyHostToDevice,s));if(size)check(cudaMemcpyAsync(receive_payload.data,host+header_bytes+mode_bytes,size,cudaMemcpyHostToDevice,s));check(cudaMemsetAsync(error.data,0,4,s));
    receive_modes<Tile><<<(tiles+255)/256,256,0,s>>>(receive_modes_buffer.data,reinterpret_cast<uint32_t*>(decode_lengths.data),tiles,n);check(cudaMemsetAsync(decode_lengths.data+tiles*4,0,4,s));check(cub::DeviceScan::ExclusiveSum(scan_temp,scan_bytes,reinterpret_cast<uint32_t*>(decode_lengths.data),reinterpret_cast<uint32_t*>(decode_offsets.data),tiles+1,s));
    restore<Tile><<<tiles,256,0,s>>>(receive_modes_buffer.data,receive_payload.data,size,reinterpret_cast<uint32_t*>(decode_offsets.data),reference.data,reference.pitch,decoded.data,decoded.pitch,w,h,dx,dy,reinterpret_cast<unsigned*>(error.data));check(cudaGetLastError());check(cudaMemcpyAsync(host_error,error.data,4,cudaMemcpyDeviceToHost,s));check(cudaStreamSynchronize(s));require(*host_error==0,"Invalid residual GPU decode");
  }
};
static std::vector<unsigned char> changed(const std::vector<unsigned char>&ref,int w,int h,int mode){
  auto out=ref;for(size_t i=0;i<out.size();++i){int prediction=cpu_prediction(ref,w,h,i,4,2);if(mode==0)continue;if(mode==6){unsigned v=unsigned(i)*0x45d9f3bu;v^=v>>16;out[i]=v&255;continue;}if(prediction<0){out[i]=unsigned(i*13+97)&255;continue;}int value=prediction;if(mode==3)value=std::clamp(value+3,0,255);if(mode==4 && i%w>unsigned(w/3)&&i%w<unsigned(w/2)&&i/w>unsigned(h/3)&&i/w<unsigned(h/2))value=255-value;if(mode==5){int other=cpu_prediction(ref,w,h,i,2,2);value=(value+std::max(0,other)+1)/2;}out[i]=value;}return out;
}
template<unsigned Tile>static void verify(int w,int h){
  Codec<Tile> c(w,h);auto ref=synthetic(w,h,0);
  for(int mode=0;mode<7;++mode){auto cur=changed(ref,w,h,mode);c.reference.upload(ref);c.current.upload(cur);int dx=(mode==0||mode==2||mode==6)?0:4,dy=dx?2:0;auto packet=c.encode(dx,dy);require(cpu_decode(packet,ref,0)==cur,"Residual CPU oracle mismatch");c.decode(packet);require(c.decoded.read()==cur,"Residual GPU reconstruction mismatch");c.payload.read(get(packet,28,4));c.wire_modes.read(c.mode_bytes);
    // Reset payload guard between differently sized fixture cases.
    check(cudaMemsetAsync(c.payload.data,0xcd,c.payload.n+64,c.timer.stream));check(cudaStreamSynchronize(c.timer.stream));
    bool rejected=false;try{cpu_decode(packet,ref,7);}catch(const std::exception&){rejected=true;}require(rejected,"Residual wrong reference accepted");
    if(packet.size()>header_bytes){auto truncated=packet;truncated.pop_back();rejected=false;try{c.decode(truncated);}catch(const std::exception&){rejected=true;}require(rejected,"Residual truncation accepted");}
  }
}
static void self_test(){bool requested=compress_modes;for(bool compression:{false,true}){compress_modes=compression;for(auto geometry:{std::pair{4,2},std::pair{6,6},std::pair{68,20},std::pair{514,66}}){verify<128>(geometry.first,geometry.second);verify<512>(geometry.first,geometry.second);}}
  Codec<128> c(1024,256);std::vector<unsigned char> ref(c.n),cur(c.n);for(size_t i=0;i<c.n;++i){ref[i]=(i>>8)&255;cur[i]=i&255;}c.reference.upload(ref);c.current.upload(cur);auto packet=c.encode(0,0);c.decode(packet);require(c.decoded.read()==cur&&cpu_decode(packet,ref,0)==cur,"Residual exhaustive byte-pair mismatch");
  for(int mode=0;mode<4;++mode){for(size_t i=0;i<c.n;++i){ref[i]=8+i%240;cur[i]=int(ref[i])+(mode==0?int(i%16)-8:mode==1?-8:mode==2?7:0);}c.reference.upload(ref);c.current.upload(cur);check(cudaMemsetAsync(c.payload.data,0xcd,c.payload.n+64,c.timer.stream));check(cudaStreamSynchronize(c.timer.stream));packet=c.encode(0,0);c.decode(packet);require(c.decoded.read()==cur&&cpu_decode(packet,ref,0)==cur,"Residual signed-nibble/constant endpoint mismatch");auto flags=c.wire_modes.read(c.mode_bytes);for(size_t t=0;t<c.tiles;++t)require(((flags[t/4]>>(2*(t%4)))&3)==unsigned(mode==0?Nibbles:mode==3?Zero:Constant),"Residual expected mode missing");c.payload.read(get(packet,28,4));
    if(get(packet,22,2)==2){auto invalid=packet;invalid[header_bytes]=invalid[header_bytes+1]=0;bool rejected=false;try{c.decode(invalid);}catch(const std::exception&){rejected=true;}require(rejected,"Zero-length mode run accepted");}
  }
  compress_modes=requested;std::puts("{\"residual_self_test\":\"pass\",\"all_65536_byte_pairs\":true,\"all_16_signed_nibbles\":true,\"cpu_decoder\":true,\"guard_and_pitch\":true,\"raw_and_rle_metadata\":true,\"wrong_reference_and_truncation_rejected\":true}");}
template<unsigned Tile>static void benchmark(){
  Codec<Tile> c(width,height);auto ref=synthetic(width,height,0);c.reference.upload(ref);const char *names[]={"static","oracle_integer_pan","pan_without_motion","pan_brightness_plus3","pan_moving_patch","pan_subpixel","scene_cut"};
  for(int round=0;round<2;++round)for(int order=0;order<7;++order){int mode=round?6-order:order;auto cur=changed(ref,width,height,mode);c.current.upload(cur);check(cudaMemsetAsync(c.payload.data,0xcd,c.payload.n+64,c.timer.stream));check(cudaStreamSynchronize(c.timer.stream));std::vector<double> encode,decode,total;size_t bytes=0;std::vector<unsigned char> last;
    for(unsigned i=0;i<90;++i){int dx=(mode==0||mode==2||mode==6)?0:4,dy=dx?2:0;auto begin=Clock::now();auto packet=c.encode(dx,dy);auto sent=Clock::now();c.decode(packet);auto done=Clock::now();if(i>=10){encode.push_back(std::chrono::duration<double,std::micro>(sent-begin).count());decode.push_back(std::chrono::duration<double,std::micro>(done-sent).count());total.push_back(std::chrono::duration<double,std::micro>(done-begin).count());}bytes=packet.size();last=std::move(packet);}
    require(c.decoded.read()==cur&&cpu_decode(last,ref,0)==cur,"Residual timed reconstruction mismatch");c.payload.read(get(last,28,4));auto flags=c.wire_modes.read(c.mode_bytes);std::array<size_t,4> counts{};for(size_t t=0;t<c.tiles;++t)++counts[(flags[t/4]>>(2*(t%4)))&3];
    std::printf("{\"codec\":\"lossless_residual_nibbles\",\"case\":\"%s\",\"tile_bytes\":%u,\"round\":%d,\"source_bytes\":%zu,\"packet_bytes\":%zu,\"mode_bytes\":%zu,\"mbps_at_120\":%.3f,\"zero_tiles\":%zu,\"constant_tiles\":%zu,\"nibble_tiles\":%zu,\"raw_tiles\":%zu,\"lossless\":true",names[mode],Tile,round,c.n,bytes,c.mode_bytes,bytes*8.*120/1e6,counts[0],counts[1],counts[2],counts[3]);quantiles("encode_cpu_packets_ready",encode);quantiles("decode_completed",decode);quantiles("local_roundtrip",total);std::puts("}");std::fflush(stdout);
  }
}
static void benchmark(){self_test();benchmark<128>();benchmark<512>();benchmark<2048>();}
} // namespace residual
