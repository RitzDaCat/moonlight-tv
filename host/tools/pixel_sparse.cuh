// SPDX-License-Identifier: GPL-3.0-only
/** Lossless sparse correction experiment over captured NV12 bytes. */
#pragma once

namespace sparse_pixels {
using Clock = std::chrono::steady_clock;
constexpr unsigned header_bytes = 40;
constexpr uint32_t magic = 0x31535052;
enum Mode : unsigned { Zero, Constant, Nibbles, Raw, Sparse };
static bool estimate_from_pixels=false;
static bool use_mean_bias=false;
struct Motion { int dx,dy,by,buv; };
struct Moments { long long sum; unsigned long long square; };
struct SumMoments {
  __device__ Moments operator()(Moments a,Moments b)const { return {a.sum+b.sum,a.square+b.square}; }
};
struct Candidate { unsigned long long error; long long sum; int index; };
struct BestCandidate {
  __device__ Candidate operator()(Candidate a,Candidate b)const {
    int ax=a.index%9-4,ay=a.index/9-4,bx=b.index%9-4,by=b.index/9-4;
    int ad=ax*ax+ay*ay,bd=bx*bx+by*by;
    return b.error<a.error || (b.error==a.error && (bd<ad || (bd==ad && b.index<a.index))) ? b : a;
  }
};

/** Search 81 even translations using 1024 luma samples and an offset-invariant SSD. */
__global__ void motion_candidates(const unsigned char *source,size_t sp,const unsigned char *reference,
                                  size_t rp,int w,int h,Candidate *candidates) {
  using Reduce=cub::BlockReduce<Moments,256>;
  __shared__ typename Reduce::TempStorage temporary;
  int dx=(int(blockIdx.x)%9-4)*2,dy=(int(blockIdx.x)/9-4)*2;
  Moments moments{0,0};
  for(unsigned sample=threadIdx.x;sample<1024;sample+=blockDim.x) {
    unsigned hash=(sample+1)*0x9e3779b9u;
    int x=8+int(hash%unsigned(w-16)),y=8+int((hash*0x85ebca6bu)%unsigned(h-16));
    int delta=int(source[size_t(y)*sp+x])-int(reference[size_t(y-dy)*rp+x-dx]);
    moments.sum+=delta; moments.square+=delta*delta;
  }
  auto sum=Reduce(temporary).Reduce(moments,SumMoments{});
  if(!threadIdx.x)candidates[blockIdx.x]={sum.square*1024-unsigned(sum.sum<0?-sum.sum:sum.sum)*static_cast<unsigned long long>(sum.sum<0?-sum.sum:sum.sum),sum.sum,int(blockIdx.x)};
}

__global__ void select_motion(const Candidate *candidates,const unsigned char *source,size_t sp,
                              const unsigned char *reference,size_t rp,int w,int h,Motion *output,bool modal_bias) {
  using Best=cub::BlockReduce<Candidate,256>;
  using Sum=cub::BlockReduce<int,256>;
  __shared__ typename Best::TempStorage best_storage;
  __shared__ typename Sum::TempStorage sum_storage;
  __shared__ Motion chosen;
  __shared__ unsigned luma_histogram[511],chroma_histogram[511];
  Candidate candidate=threadIdx.x<81 ? candidates[threadIdx.x] : Candidate{~0ull,0,40};
  auto best=Best(best_storage).Reduce(candidate,BestCandidate{});
  if(!threadIdx.x) {
    bool confident=best.error<1600ull*1024*1024;
    chosen={confident?(best.index%9-4)*2:0,confident?(best.index/9-4)*2:0,
            confident?__float2int_rn(float(best.sum)/1024):0,0};
  }
  __syncthreads();
  unsigned uv_hash=(threadIdx.x+1)*0x9e3779b9u;
  int uv_x=8+int(uv_hash%unsigned(w-16)),uv_y=4+int((uv_hash*0x85ebca6bu)%unsigned(h/2-8));
  int uv_delta=int(source[size_t(h+uv_y)*sp+uv_x])-int(reference[size_t(h+uv_y-chosen.dy/2)*rp+uv_x-chosen.dx]);
  if(!modal_bias) {
    int sum=Sum(sum_storage).Sum(uv_delta);
    if(!threadIdx.x) { chosen.buv=__float2int_rn(float(sum)/256); *output=chosen; }
    return;
  }
  for(unsigned i=threadIdx.x;i<511;i+=blockDim.x) { luma_histogram[i]=0;chroma_histogram[i]=0; }
  __syncthreads();
  for(unsigned sample=threadIdx.x;sample<1024;sample+=blockDim.x) {
    unsigned hash=(sample+1)*0x9e3779b9u;
    int x=8+int(hash%unsigned(w-16)),y=8+int((hash*0x85ebca6bu)%unsigned(h-16));
    int difference=int(source[size_t(y)*sp+x])-int(reference[size_t(y-chosen.dy)*rp+x-chosen.dx]);
    atomicAdd(luma_histogram+difference+255,1u);
  }
  atomicAdd(chroma_histogram+uv_delta+255,1u);
  __syncthreads();
  if(!threadIdx.x) {
    // A moving patch must not bias the unchanged majority of the image.
    int luma=255,chroma=255;
    for(int i=0;i<511;++i) {
      if(luma_histogram[i]>luma_histogram[luma] || (luma_histogram[i]==luma_histogram[luma] && abs(i-255)<abs(luma-255)))luma=i;
      if(chroma_histogram[i]>chroma_histogram[chroma] || (chroma_histogram[i]==chroma_histogram[chroma] && abs(i-255)<abs(chroma-255)))chroma=i;
    }
    chosen.by=luma_histogram[luma]>=512 ? luma-255 : 0;
    chosen.buv=chroma_histogram[chroma]>=128 ? chroma-255 : 0;
    *output=chosen;
  }
}

__device__ int prediction(const unsigned char *reference, size_t pitch, int w, int h,
                          size_t index, int dx, int dy, int bias_y, int bias_uv) {
  int value = residual::predicted(reference, pitch, w, h, index, dx, dy);
  if (value < 0) return -1;
  return min(255, max(0, value + (index < size_t(w)*h ? bias_y : bias_uv)));
}

struct Summary { int lo, hi; unsigned changed; };
struct Combine {
  __device__ Summary operator()(Summary a, Summary b) const {
    return {min(a.lo,b.lo), max(a.hi,b.hi), a.changed+b.changed};
  }
};

template<unsigned Tile>
__global__ void classify(const unsigned char *source, size_t sp, const unsigned char *reference,
                         size_t rp, int w, int h, int dx, int dy, int by, int buv,
                         uint16_t *directory, uint32_t *lengths,const Motion *inferred) {
  if(inferred) { dx=inferred->dx;dy=inferred->dy;by=inferred->by;buv=inferred->buv; }
  using Reduce = cub::BlockReduce<Summary,256>;
  __shared__ typename Reduce::TempStorage temporary;
  size_t first=size_t(blockIdx.x)*Tile, n=size_t(w)*h*3/2;
  unsigned active=unsigned(min(size_t(Tile),n-first));
  Summary local{256,-256,0};
  for (unsigned j=threadIdx.x; j<active; j+=blockDim.x) {
    size_t i=first+j;
    int value=source[(i/w)*sp+i%w];
    int p=prediction(reference,rp,w,h,i,dx,dy,by,buv);
    local.changed += p<0 || value!=p;
    local.lo=min(local.lo,p<0 ? -256 : value-p);
    local.hi=max(local.hi,p<0 ? 256 : value-p);
  }
  auto result=Reduce(temporary).Reduce(local,Combine{});
  if (!threadIdx.x) {
    unsigned mode=Raw, length=active;
    if (result.lo==0 && result.hi==0) { mode=Zero; length=0; }
    else if (result.lo==result.hi && result.lo>=-8 && result.hi<=7) { mode=Constant; length=1; }
    else if (result.lo>=-8 && result.hi<=7) { mode=Nibbles; length=(active+1)/2; }
    unsigned sparse_length=2+3*result.changed;
    if (sparse_length<length) { mode=Sparse; length=sparse_length; }
    directory[blockIdx.x]=uint16_t((length<<3)|mode);
    lengths[blockIdx.x]=length;
  }
}

template<unsigned Tile>
__global__ void pack(const unsigned char *source, size_t sp, const unsigned char *reference,
                     size_t rp, int w, int h, int dx, int dy, int by, int buv,
                     const uint16_t *directory, const uint32_t *offsets, unsigned char *payload,const Motion *inferred) {
  if(inferred) { dx=inferred->dx;dy=inferred->dy;by=inferred->by;buv=inferred->buv; }
  using Scan=cub::BlockScan<unsigned,256>;
  __shared__ typename Scan::TempStorage temporary;
  __shared__ unsigned preceding;
  unsigned descriptor=directory[blockIdx.x], mode=descriptor&7;
  size_t first=size_t(blockIdx.x)*Tile,n=size_t(w)*h*3/2;
  unsigned active=unsigned(min(size_t(Tile),n-first));
  auto output=payload+offsets[blockIdx.x];
  if (mode==Zero) return;
  if (mode==Raw) {
    for (unsigned j=threadIdx.x;j<active;j+=blockDim.x) {
      size_t i=first+j; output[j]=source[(i/w)*sp+i%w];
    }
    return;
  }
  if (mode==Constant) {
    if (!threadIdx.x) output[0]=source[(first/w)*sp+first%w]-prediction(reference,rp,w,h,first,dx,dy,by,buv)+8;
    return;
  }
  if (mode==Nibbles) {
    for (unsigned pair=threadIdx.x;pair<(active+1)/2;pair+=blockDim.x) {
      unsigned value=0;
      for (unsigned k=0;k<2 && pair*2+k<active;++k) {
        size_t i=first+pair*2+k;
        int delta=source[(i/w)*sp+i%w]-prediction(reference,rp,w,h,i,dx,dy,by,buv);
        value|=unsigned(delta+8)<<(4*k);
      }
      output[pair]=value;
    }
    return;
  }
  unsigned count=((descriptor>>3)-2)/3;
  if (!threadIdx.x) { output[0]=count&255; output[1]=count>>8; preceding=0; }
  __syncthreads();
  for (unsigned chunk=0;chunk<active;chunk+=blockDim.x) {
    unsigned j=chunk+threadIdx.x, changed=0, prefix=0, total=0;
    unsigned char value=0;
    if (j<active) {
      size_t i=first+j; value=source[(i/w)*sp+i%w];
      int p=prediction(reference,rp,w,h,i,dx,dy,by,buv);
      changed=p<0 || int(value)!=p;
    }
    Scan(temporary).ExclusiveSum(changed,prefix,total);
    if (changed) {
      unsigned address=2+3*(preceding+prefix);
      output[address]=j&255; output[address+1]=j>>8; output[address+2]=value;
    }
    __syncthreads();
    if (!threadIdx.x) preceding+=total;
    __syncthreads();
  }
}

__global__ void receive_lengths(const uint16_t *directory,uint32_t *lengths,unsigned tiles) {
  unsigned t=blockIdx.x*blockDim.x+threadIdx.x;
  if (t<tiles) lengths[t]=directory[t]>>3;
}

template<unsigned Tile>
__global__ void restore(const uint16_t *directory,const uint32_t *offsets,const unsigned char *payload,
                        const unsigned char *reference,size_t rp,unsigned char *output,size_t op,
                        int w,int h,int dx,int dy,int by,int buv,unsigned *error) {
  __shared__ unsigned char undefined[Tile];
  unsigned tile=blockIdx.x, mode=directory[tile]&7;
  size_t first=size_t(tile)*Tile,n=size_t(w)*h*3/2;
  unsigned active=unsigned(min(size_t(Tile),n-first));
  const auto data=payload+offsets[tile];
  for (unsigned j=threadIdx.x;j<active;j+=blockDim.x) {
    size_t i=first+j; int value=0;
    if (mode==Raw) value=data[j];
    else {
      int p=prediction(reference,rp,w,h,i,dx,dy,by,buv);
      undefined[j]=p<0;
      if (mode==Sparse) value=max(0,p);
      else if (p<0) { atomicOr(error,1u); continue; }
      else {
        int delta=mode==Zero ? 0 : mode==Constant ? int(data[0])-8 : int((data[j/2]>>(4*(j%2)))&15)-8;
        value=p+delta;
        if (value<0 || value>255) { atomicOr(error,2u); continue; }
      }
    }
    output[(i/w)*op+i%w]=value;
  }
  if (mode!=Sparse) return;
  __syncthreads();
  unsigned count=(unsigned(data[0])|(unsigned(data[1])<<8));
  for (unsigned k=threadIdx.x;k<count;k+=blockDim.x) {
    unsigned p=2+3*k,j=unsigned(data[p])|(unsigned(data[p+1])<<8);
    // Host-side packet parsing checked unique, ordered indices and every bound.
    size_t i=first+j; output[(i/w)*op+i%w]=data[p+2]; undefined[j]=0;
  }
  __syncthreads();
  for (unsigned j=threadIdx.x;j<active;j+=blockDim.x) if (undefined[j]) atomicOr(error,4u);
}

using residual::get;
using residual::put;
struct Parsed {
  unsigned w,h,tile,payload_offset,payload_bytes,frame;
  int dx,dy,by,buv;
  std::vector<uint16_t> directory;
};

static Parsed parse(const std::vector<unsigned char>& packet,unsigned expected_reference) {
  require(packet.size()>=header_bytes && get(packet,0,4)==magic,"Bad sparse packet header");
  unsigned version=get(packet,22,2),directory_bytes=get(packet,24,4),payload_bytes=get(packet,28,4);
  require((version==3 || version==4) && get(packet,36,4)==0,"Unsupported sparse packet version/flags");
  Parsed p{get(packet,4,2),get(packet,6,2),get(packet,20,2),header_bytes+directory_bytes,payload_bytes,get(packet,8,4),
           int16_t(get(packet,16,2)),int16_t(get(packet,18,2)),int16_t(get(packet,32,2)),int16_t(get(packet,34,2)),{}};
  require(p.w && p.h && p.w<=8192 && p.h<=8192 && p.w%2==0 && p.h%2==0 &&
          (p.tile==128 || p.tile==512 || p.tile==2048),"Invalid sparse geometry");
  require(p.dx%2==0 && p.dy%2==0 && p.by>=-255 && p.by<=255 && p.buv>=-255 && p.buv<=255,"Invalid sparse predictor");
  require(get(packet,12,4)==expected_reference && p.frame>expected_reference,"Missing or noncausal sparse reference");
  size_t n=size_t(p.w)*p.h*3/2,tiles=(n+p.tile-1)/p.tile;
  require(payload_bytes<=n && size_t(header_bytes)+directory_bytes+payload_bytes==packet.size(),"Sparse packet length mismatch");
  p.directory.reserve(tiles);
  if (version==3) {
    require(directory_bytes==2*tiles,"Sparse literal directory length mismatch");
    for (unsigned i=header_bytes;i<p.payload_offset;i+=2) p.directory.push_back(get(packet,i,2));
  } else {
    require(directory_bytes%4==0,"Sparse RLE directory length mismatch");
    for (unsigned i=header_bytes;i<p.payload_offset;i+=4) {
      unsigned count=get(packet,i,2),value=get(packet,i+2,2);
      require(count && count<=tiles-p.directory.size(),"Invalid sparse directory run");
      p.directory.insert(p.directory.end(),count,value);
    }
    require(p.directory.size()==tiles,"Incomplete sparse directory");
  }
  size_t cursor=p.payload_offset;
  for (size_t t=0;t<tiles;++t) {
    unsigned mode=p.directory[t]&7,length=p.directory[t]>>3;
    size_t active=std::min(size_t(p.tile),n-t*p.tile);
    require(mode<=Sparse && cursor+length<=packet.size(),"Invalid sparse mode/payload bound");
    if (mode==Zero) require(length==0,"Invalid zero payload");
    if (mode==Constant) require(length==1 && packet[cursor]<=15,"Invalid constant payload");
    if (mode==Nibbles) require(length==(active+1)/2,"Invalid nibble payload");
    if (mode==Raw) require(length==active,"Invalid raw payload");
    if (mode==Sparse) {
      require(length>=2 && (length-2)%3==0,"Invalid sparse index payload");
      unsigned count=get(packet,cursor,2);
      require(count==(length-2)/3 && count<=active,"Sparse count does not match directory");
      unsigned previous=0;
      for (unsigned k=0;k<count;++k) {
        unsigned index=get(packet,cursor+2+3*k,2);
        require(index<active && (!k || index>previous),"Sparse indices must be in bounds, unique and ordered");
        previous=index;
      }
    }
    cursor+=length;
  }
  require(cursor==packet.size(),"Trailing sparse payload");
  return p;
}

static int cpu_prediction(const std::vector<unsigned char>& reference,const Parsed& p,size_t i) {
  int value=residual::cpu_prediction(reference,p.w,p.h,i,p.dx,p.dy);
  return value<0 ? -1 : std::clamp(value+(i<size_t(p.w)*p.h ? p.by : p.buv),0,255);
}

static std::vector<unsigned char> cpu_decode(const std::vector<unsigned char>& packet,
                                           const std::vector<unsigned char>& reference,unsigned expected_reference) {
  auto p=parse(packet,expected_reference);
  size_t n=size_t(p.w)*p.h*3/2,cursor=p.payload_offset;
  require(reference.size()==n,"Sparse reference size mismatch");
  std::vector<unsigned char> out(n),undefined(p.tile);
  for (size_t t=0;t<p.directory.size();++t) {
    unsigned mode=p.directory[t]&7,length=p.directory[t]>>3;
    size_t first=t*p.tile,active=std::min(size_t(p.tile),n-first);
    for (size_t j=0;j<active;++j) {
      if (mode==Raw) { out[first+j]=packet[cursor+j]; continue; }
      int value=cpu_prediction(reference,p,first+j);
      undefined[j]=value<0;
      if (mode==Sparse) { out[first+j]=std::max(0,value); continue; }
      require(value>=0,"Sparse predictor outside reference");
      int delta=mode==Zero ? 0 : mode==Constant ? int(packet[cursor])-8 : int((packet[cursor+j/2]>>(4*(j%2)))&15)-8;
      require(value+delta>=0 && value+delta<=255,"Sparse arithmetic overflow");
      out[first+j]=value+delta;
    }
    if (mode==Sparse) {
      unsigned count=get(packet,cursor,2);
      for (unsigned k=0;k<count;++k) {
        unsigned j=get(packet,cursor+2+3*k,2);
        out[first+j]=packet[cursor+2+3*k+2]; undefined[j]=0;
      }
      require(std::none_of(undefined.begin(),undefined.begin()+active,[](auto v){return v!=0;}),"Sparse border correction missing");
    }
    cursor+=length;
  }
  return out;
}

template<unsigned Tile> struct Codec {
  int w,h;
  size_t n,tiles,directory_bytes;
  Image current,reference,decoded;
  Bytes directory,lengths,offsets,payload,receive_directory,receive_payload,receive_lengths_buffer,receive_offsets,error,candidates,motion;
  Timer timer;
  void *scan_temp=nullptr;
  size_t scan_bytes=0;
  unsigned char *host=nullptr;
  uint32_t *host_size=nullptr,*host_error=nullptr;
  Motion *host_motion=nullptr;
  unsigned frame_id=1,reference_id=0;

  Codec(int w,int h):w(w),h(h),n(size_t(w)*h*3/2),tiles((n+Tile-1)/Tile),directory_bytes(tiles*2),
      current(w,h),reference(w,h),decoded(w,h),directory(directory_bytes),lengths((tiles+1)*4),offsets((tiles+1)*4),payload(n),
      receive_directory(directory_bytes),receive_payload(n),receive_lengths_buffer((tiles+1)*4),receive_offsets((tiles+1)*4),error(4),candidates(81*sizeof(Candidate)),motion(sizeof(Motion)) {
    try {
      check(cub::DeviceScan::ExclusiveSum(nullptr,scan_bytes,reinterpret_cast<uint32_t*>(lengths.data),reinterpret_cast<uint32_t*>(offsets.data),tiles+1,timer.stream));
      check(cudaMalloc(&scan_temp,scan_bytes));
      check(cudaMallocHost(&host,directory_bytes+n));
      check(cudaMallocHost(&host_size,4)); check(cudaMallocHost(&host_error,4));
      check(cudaMallocHost(&host_motion,sizeof(Motion)));
    } catch (...) { clear(); throw; }
  }
  void clear() { if(scan_temp)cudaFree(scan_temp); if(host)cudaFreeHost(host); if(host_size)cudaFreeHost(host_size); if(host_error)cudaFreeHost(host_error); if(host_motion)cudaFreeHost(host_motion); }
  ~Codec() { clear(); }

  std::vector<unsigned char> encode(int dx,int dy,int by=0,int buv=0,bool estimate=false) {
    require(dx%2==0 && dy%2==0 && frame_id>reference_id,"Invalid sparse encoding predictor/reference");
    auto stream=timer.stream;
    const Motion *inferred=nullptr;
    if(estimate) {
      inferred=reinterpret_cast<Motion*>(motion.data);
      if(w>32 && h>32) {
        motion_candidates<<<81,256,0,stream>>>(current.data,current.pitch,reference.data,reference.pitch,w,h,reinterpret_cast<Candidate*>(candidates.data));
        select_motion<<<1,256,0,stream>>>(reinterpret_cast<Candidate*>(candidates.data),current.data,current.pitch,reference.data,reference.pitch,w,h,reinterpret_cast<Motion*>(motion.data),!use_mean_bias);
      } else check(cudaMemsetAsync(motion.data,0,sizeof(Motion),stream));
    }
    classify<Tile><<<tiles,256,0,stream>>>(current.data,current.pitch,reference.data,reference.pitch,w,h,dx,dy,by,buv,
        reinterpret_cast<uint16_t*>(directory.data),reinterpret_cast<uint32_t*>(lengths.data),inferred);
    check(cudaMemsetAsync(lengths.data+tiles*4,0,4,stream));
    check(cub::DeviceScan::ExclusiveSum(scan_temp,scan_bytes,reinterpret_cast<uint32_t*>(lengths.data),reinterpret_cast<uint32_t*>(offsets.data),tiles+1,stream));
    pack<Tile><<<tiles,256,0,stream>>>(current.data,current.pitch,reference.data,reference.pitch,w,h,dx,dy,by,buv,
        reinterpret_cast<uint16_t*>(directory.data),reinterpret_cast<uint32_t*>(offsets.data),payload.data,inferred);
    check(cudaGetLastError());
    check(cudaMemcpyAsync(host_size,offsets.data+tiles*4,4,cudaMemcpyDeviceToHost,stream));
    if(estimate)check(cudaMemcpyAsync(host_motion,motion.data,sizeof(Motion),cudaMemcpyDeviceToHost,stream));
    check(cudaStreamSynchronize(stream)); require(*host_size<=n,"Sparse payload exceeds source");
    if(estimate) { dx=host_motion->dx;dy=host_motion->dy;by=host_motion->by;buv=host_motion->buv; }
    check(cudaMemcpyAsync(host,directory.data,directory_bytes,cudaMemcpyDeviceToHost,stream));
    if(*host_size)check(cudaMemcpyAsync(host+directory_bytes,payload.data,*host_size,cudaMemcpyDeviceToHost,stream));
    check(cudaStreamSynchronize(stream));
    std::vector<unsigned char> rle;
    const auto descriptors=reinterpret_cast<uint16_t*>(host);
    for(size_t i=0;i<tiles;) {
      size_t end=i+1; while(end<tiles && end-i<65535 && descriptors[end]==descriptors[i])++end;
      for(unsigned value:{unsigned(end-i),unsigned(descriptors[i])}) { rle.push_back(value&255); rle.push_back(value>>8); }
      i=end;
    }
    bool compressed=rle.size()<directory_bytes;
    size_t wire_directory=compressed ? rle.size() : directory_bytes;
    std::vector<unsigned char> packet(header_bytes+wire_directory+*host_size);
    if(compressed)std::copy(rle.begin(),rle.end(),packet.begin()+header_bytes);
    else for(size_t t=0;t<tiles;++t)put(packet,header_bytes+2*t,descriptors[t],2);
    std::copy_n(host+directory_bytes,*host_size,packet.begin()+header_bytes+wire_directory);
    put(packet,0,magic,4); put(packet,4,w,2); put(packet,6,h,2); put(packet,8,frame_id,4); put(packet,12,reference_id,4);
    put(packet,16,uint16_t(dx),2); put(packet,18,uint16_t(dy),2); put(packet,20,Tile,2); put(packet,22,compressed?4:3,2);
    put(packet,24,wire_directory,4); put(packet,28,*host_size,4); put(packet,32,uint16_t(by),2); put(packet,34,uint16_t(buv),2);
    return packet;
  }

  void decode(const std::vector<unsigned char>& packet) {
    auto parsed=parse(packet,reference_id);
    require(parsed.w==unsigned(w) && parsed.h==unsigned(h) && parsed.tile==Tile,"Sparse receiver geometry mismatch");
    auto stream=timer.stream;
    std::copy(parsed.directory.begin(),parsed.directory.end(),reinterpret_cast<uint16_t*>(host));
    std::copy_n(packet.data()+parsed.payload_offset,parsed.payload_bytes,host+directory_bytes);
    check(cudaMemcpyAsync(receive_directory.data,host,directory_bytes,cudaMemcpyHostToDevice,stream));
    if(parsed.payload_bytes)check(cudaMemcpyAsync(receive_payload.data,host+directory_bytes,parsed.payload_bytes,cudaMemcpyHostToDevice,stream));
    receive_lengths<<<(tiles+255)/256,256,0,stream>>>(reinterpret_cast<uint16_t*>(receive_directory.data),reinterpret_cast<uint32_t*>(receive_lengths_buffer.data),tiles);
    check(cudaMemsetAsync(receive_lengths_buffer.data+tiles*4,0,4,stream));
    check(cub::DeviceScan::ExclusiveSum(scan_temp,scan_bytes,reinterpret_cast<uint32_t*>(receive_lengths_buffer.data),reinterpret_cast<uint32_t*>(receive_offsets.data),tiles+1,stream));
    check(cudaMemsetAsync(error.data,0,4,stream));
    restore<Tile><<<tiles,256,0,stream>>>(reinterpret_cast<uint16_t*>(receive_directory.data),reinterpret_cast<uint32_t*>(receive_offsets.data),receive_payload.data,
        reference.data,reference.pitch,decoded.data,decoded.pitch,w,h,parsed.dx,parsed.dy,parsed.by,parsed.buv,reinterpret_cast<unsigned*>(error.data));
    check(cudaGetLastError()); check(cudaMemcpyAsync(host_error,error.data,4,cudaMemcpyDeviceToHost,stream));
    check(cudaStreamSynchronize(stream)); require(!*host_error,"Sparse GPU reconstruction rejected payload");
  }
};

template<unsigned Tile> static void verify(int w,int h) {
  Codec<Tile> codec(w,h); auto reference=synthetic(w,h,0);
  codec.reference.upload(reference);
  for(int mode=0;mode<7;++mode) {
    auto source=residual::changed(reference,w,h,mode); codec.current.upload(source);
    int dx=(mode==0 || mode==2 || mode==6) ? 0 : 4,dy=dx?2:0;
    check(cudaMemsetAsync(codec.payload.data,0xcd,codec.payload.n+64,codec.timer.stream));
    auto packet=codec.encode(dx,dy); codec.decode(packet);
    require(cpu_decode(packet,reference,0)==source && codec.decoded.read()==source,"Sparse reconstruction differs from source");
    codec.payload.read(get(packet,28,4)); codec.directory.read(codec.directory_bytes);
    auto reject=[&](const std::vector<unsigned char>& invalid) {
      bool rejected=false; try { codec.decode(invalid); } catch(const std::exception&) { rejected=true; }
      require(rejected,"Malformed sparse packet accepted");
    };
    auto invalid=packet; invalid.pop_back(); reject(invalid);
    invalid=packet; put(invalid,12,9,4); reject(invalid);
    invalid=packet; put(invalid,36,1,4); reject(invalid);
    if(get(packet,22,2)==4) { invalid=packet; put(invalid,header_bytes,0,2); reject(invalid); }
  }
}

static void self_test() {
  for(auto size:{std::pair{4,2},std::pair{6,6},std::pair{68,20},std::pair{514,66}}) {
    verify<128>(size.first,size.second); verify<512>(size.first,size.second); verify<2048>(size.first,size.second);
  }
  Codec<128> codec(1024,256);
  std::vector<unsigned char> reference(codec.n),source(codec.n);
  for(size_t i=0;i<codec.n;++i) { reference[i]=(i>>8)&255; source[i]=i&255; }
  codec.reference.upload(reference); codec.current.upload(source);
  auto packet=codec.encode(0,0); codec.decode(packet);
  require(codec.decoded.read()==source && cpu_decode(packet,reference,0)==source,"Sparse exhaustive byte pairs failed");
  for(int mode=0;mode<4;++mode) {
    for(size_t i=0;i<codec.n;++i) { reference[i]=128; source[i]=mode==0 ? 128 : mode==1 ? 131 : mode==2 ? 128+int(i%16)-8 : (i%128==127?200:128); }
    codec.reference.upload(reference); codec.current.upload(source); packet=codec.encode(0,0); codec.decode(packet);
    require(codec.decoded.read()==source && cpu_decode(packet,reference,0)==source,"Sparse mode or nibble endpoint failure");
    auto parsed=parse(packet,0); unsigned expected=mode==0?Zero:mode==1?Constant:mode==2?Nibbles:Sparse;
    for(auto entry:parsed.directory)require((entry&7)==expected,"Sparse expected mode not exercised");
    if(mode==3) {
      auto invalid=packet; put(invalid,parsed.payload_offset+2,128,2);
      bool rejected=false;try { codec.decode(invalid); } catch(const std::exception&) { rejected=true; }
      require(rejected,"Out-of-bounds sparse index accepted");
      invalid=packet; put(invalid,parsed.payload_offset,2,2); rejected=false;
      try { codec.decode(invalid); } catch(const std::exception&) { rejected=true; }
      require(rejected,"Invalid sparse count accepted");
    }
  }
  // Receiver owns its reference and computes offsets from the packet directory.
  Codec<128> sender(68,20),receiver(68,20); reference=synthetic(68,20,0);
  sender.reference.upload(reference); receiver.reference.upload(reference);
  for(unsigned frame=1;frame<=16;++frame) {
    source=residual::changed(reference,68,20,1); sender.current.upload(source);
    sender.frame_id=frame; sender.reference_id=receiver.reference_id=frame-1;
    packet=sender.encode(4,2); receiver.decode(packet);
    auto actual=receiver.decoded.read(); require(actual==source && cpu_decode(packet,reference,frame-1)==source,"Sparse causal chain failed");
    receiver.reference.upload(actual); sender.reference.upload(source); reference=std::move(source);
  }
  Codec<512> estimator(1024,256);reference=synthetic(1024,256,0);estimator.reference.upload(reference);
  for(int mode:{0,1,3,4,5,6}) {
    source=residual::changed(reference,1024,256,mode);estimator.current.upload(source);
    packet=estimator.encode(0,0,0,0,true);estimator.decode(packet);
    require(estimator.decoded.read()==source && cpu_decode(packet,reference,0)==source,"Pixel-derived predictor reconstruction failed");
    auto parsed=parse(packet,0);
    if(mode==1 || mode==3)require(parsed.dx==4 && parsed.dy==2 && parsed.by==(mode==3?3:0),"Pixel-derived translation/brightness estimate failed");
    if(mode==4 && !use_mean_bias)require(parsed.dx==4 && parsed.dy==2 && parsed.by==0 && parsed.buv==0,"Moving patch biased the unchanged background");
  }
  std::puts("{\"sparse_self_test\":\"pass\",\"all_65536_byte_pairs\":true,\"all_signed_nibbles\":true,\"all_modes\":true,\"independent_cpu_decoder\":true,\"receiver_owned_reference\":true,\"causal_chain_frames\":16,\"pixel_motion_and_brightness\":true,\"malformed_packets_rejected\":true,\"pitch_and_guards\":true}");
}

template<unsigned Tile> static void benchmark() {
  Codec<Tile> codec(width,height); auto reference=synthetic(width,height,0); codec.reference.upload(reference);
  const char *names[]={"static","oracle_integer_pan","pan_without_motion","pan_brightness_plus3","pan_moving_patch","pan_subpixel","scene_cut"};
  for(int round=0;round<2;++round)for(int order=0;order<7;++order) {
    int mode=round?6-order:order; auto source=residual::changed(reference,width,height,mode); codec.current.upload(source);
    check(cudaMemsetAsync(codec.payload.data,0xcd,codec.payload.n+64,codec.timer.stream));
    check(cudaStreamSynchronize(codec.timer.stream));
    int dx=(mode==0 || mode==2 || mode==6)?0:4,dy=dx?2:0;
    std::vector<double> encode,decode,total;std::vector<unsigned char> packet;
    for(unsigned iteration=0;iteration<90;++iteration) {
      auto start=Clock::now(); packet=codec.encode(dx,dy,0,0,estimate_from_pixels && mode!=2); auto sent=Clock::now(); codec.decode(packet); auto completed=Clock::now();
      if(iteration>=10) {
        encode.push_back(std::chrono::duration<double,std::micro>(sent-start).count());
        decode.push_back(std::chrono::duration<double,std::micro>(completed-sent).count());
        total.push_back(std::chrono::duration<double,std::micro>(completed-start).count());
      }
    }
    require(codec.decoded.read()==source && cpu_decode(packet,reference,0)==source,"Sparse timed reconstruction mismatch");
    codec.payload.read(get(packet,28,4)); auto parsed=parse(packet,0); std::array<size_t,5> counts{};
    for(auto entry:parsed.directory)++counts[entry&7];
    std::printf("{\"codec\":\"lossless_sparse_pixels\",\"case\":\"%s\",\"tile_bytes\":%u,\"round\":%d,\"source_bytes\":%zu,\"packet_bytes\":%zu,\"directory_bytes\":%u,\"payload_bytes\":%u,\"mbps_at_120\":%.3f,\"zero_tiles\":%zu,\"constant_tiles\":%zu,\"nibble_tiles\":%zu,\"raw_tiles\":%zu,\"sparse_tiles\":%zu,\"lossless\":true",
      names[mode],Tile,round,codec.n,packet.size(),get(packet,24,4),get(packet,28,4),packet.size()*8.*120/1e6,counts[0],counts[1],counts[2],counts[3],counts[4]);
    std::printf(",\"predictor\":\"%s\",\"bias_policy\":\"%s\",\"dx\":%d,\"dy\":%d,\"luma_bias\":%d,\"chroma_bias\":%d",estimate_from_pixels && mode!=2?"pixel-derived":dx?"oracle":"none",use_mean_bias?"mean":"modal",parsed.dx,parsed.dy,parsed.by,parsed.buv);
    quantiles("encode_cpu_packets_ready",encode);quantiles("decode_completed",decode);quantiles("local_roundtrip",total);std::puts("}");std::fflush(stdout);
  }
}
static void benchmark() { self_test(); benchmark<128>();benchmark<512>();benchmark<2048>(); }
} // namespace sparse_pixels
