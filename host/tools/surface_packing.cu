// SPDX-License-Identifier: GPL-3.0-only
/** Synthetic, opt-in nibble and coordinate-folding experiments. No host integration. */
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static constexpr int width = 3840, height = 2160, fixture_frames = 4;
static constexpr double strength = 2.;
static const std::vector<std::string> modes = {
  "copy", "nibble_lossless", "nibble4", "chroma4", "row_fold", "uniform_half", "hyperbolic_half"};
static void check(cudaError_t e) { if (e != cudaSuccess) throw std::runtime_error(cudaGetErrorString(e)); }
static void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
struct FileCloser { void operator()(FILE *f) const { std::fclose(f); } };
using File = std::unique_ptr<FILE, FileCloser>;

struct Image {
  unsigned char *data = nullptr;
  size_t pitch = 0;
  int w, h;
  Image(int w, int h) : w(w), h(h) {
    check(cudaMallocPitch(&data, &pitch, w, h * 3 / 2));
    // Include pitched padding in the correctness checks.
    if (auto e = cudaMemset(data, 0xcd, pitch * h * 3 / 2); e != cudaSuccess) {
      cudaFree(data); throw std::runtime_error(cudaGetErrorString(e));
    }
  }
  ~Image() { cudaFree(data); }
  Image(const Image &) = delete;
  void upload(const std::vector<unsigned char> &v) {
    require(v.size() == size_t(w) * h * 3 / 2, "wrong image length");
    check(cudaMemcpy2D(data, pitch, v.data(), w, w, h * 3 / 2, cudaMemcpyHostToDevice));
    check(cudaStreamSynchronize(nullptr)); // Setup/readiness fence, outside every timed span.
  }
  std::vector<unsigned char> read() const {
    std::vector<unsigned char> padded(pitch * h * 3 / 2), v(size_t(w) * h * 3 / 2);
    check(cudaMemcpy(padded.data(), data, padded.size(), cudaMemcpyDeviceToHost));
    for (int y = 0; y < h * 3 / 2; ++y) {
      std::copy_n(padded.data() + size_t(y) * pitch, w, v.data() + size_t(y) * w);
      for (size_t x = w; x < pitch; ++x)
        require(padded[size_t(y) * pitch + x] == 0xcd, "image padding overwritten");
    }
    return v;
  }
};
struct Bytes {
  unsigned char *data = nullptr;
  size_t n;
  explicit Bytes(size_t n) : n(n) {
    check(cudaMalloc(&data, n + 64));
    if (auto e = cudaMemset(data, 0xcd, n + 64); e != cudaSuccess) {
      cudaFree(data); throw std::runtime_error(cudaGetErrorString(e));
    }
  }
  ~Bytes() { cudaFree(data); }
  Bytes(const Bytes &) = delete;
  std::vector<unsigned char> read(size_t used) const {
    std::vector<unsigned char> v(n + 64);
    check(cudaMemcpy(v.data(), data, v.size(), cudaMemcpyDeviceToHost));
    for (size_t i = used; i < v.size(); ++i) require(v[i] == 0xcd, "packed buffer overrun");
    v.resize(used); return v;
  }
};
struct Axis { float position, footprint; };
struct Maps {
  Axis *x = nullptr, *y = nullptr;
  Maps(int iw, int ih, int ow, int oh, double k, bool inverse) {
    std::vector<Axis> xx, yy;
    auto add = [=](std::vector<Axis> &a, int in, int out) {
      for (int p = 0; p < out; ++p) {
        double u = 2. * (p + .5) / out - 1., v = u, derivative = 1.;
        if (k) {
          if (inverse) { v = std::asinh(u * std::sinh(k)) / k; derivative = std::sinh(k) / (k * std::sqrt(1 + std::pow(u * std::sinh(k), 2))); }
          else { v = std::sinh(k * u) / std::sinh(k); derivative = k * std::cosh(k * u) / std::sinh(k); }
        }
        a.push_back({float((v + 1) * in * .5 - .5), float(derivative * in / out)});
      }
    };
    add(xx, iw, ow); add(xx, iw / 2, ow / 2);
    add(yy, ih, oh); add(yy, ih / 2, oh / 2);
    try {
      check(cudaMalloc(&x, xx.size() * sizeof(Axis))); check(cudaMalloc(&y, yy.size() * sizeof(Axis)));
      check(cudaMemcpy(x, xx.data(), xx.size() * sizeof(Axis), cudaMemcpyHostToDevice));
      check(cudaMemcpy(y, yy.data(), yy.size() * sizeof(Axis), cudaMemcpyHostToDevice));
    } catch (...) { cudaFree(x); cudaFree(y); throw; }
  }
  ~Maps() { cudaFree(x); cudaFree(y); }
  Maps(const Maps &) = delete;
};
struct Timer {
  cudaStream_t stream = nullptr;
  cudaEvent_t begin = nullptr, middle = nullptr, end = nullptr;
  Timer() {
    try {
      check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
      check(cudaEventCreate(&begin)); check(cudaEventCreate(&middle)); check(cudaEventCreate(&end));
    } catch (...) { clear(); throw; }
  }
  void clear() { if (begin) cudaEventDestroy(begin); if (middle) cudaEventDestroy(middle); if (end) cudaEventDestroy(end); if (stream) cudaStreamDestroy(stream); }
  ~Timer() { clear(); }
};
// Y: 16 equally spaced legal-range levels. UV: preserve neutral 128 exactly.
__device__ static unsigned char quantize(unsigned char v, bool uv) {
  int a = max(16, min(uv ? 240 : 235, int(v)));
  return uv ? (a < 128 ? 8 - (128 - a + 7) / 14 : 8 + (a - 128 + 8) / 16)
            : ((a - 16) * 15 + 109) / 219;
}
__device__ static unsigned char expand(unsigned char q, bool uv) {
  return uv ? (q < 8 ? 16 + 14 * q : 128 + 16 * (q - 8)) : 16 + (219 * q + 7) / 15;
}
// A pair of arbitrary bytes needs TWO packed nibble bytes to remain lossless.
template<int policy> __global__ void pack_nibbles(const unsigned char *src, size_t pitch, int w, int h, unsigned char *packed) {
  int x = 2 * (blockIdx.x * blockDim.x + threadIdx.x), y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h * 3 / 2) return;
  unsigned a = src[size_t(y) * pitch + x], b = src[size_t(y) * pitch + x + 1];
  size_t p = size_t(y) * (w / 2) + x / 2, half = size_t(w) * h * 3 / 4;
  if constexpr (policy == 0) {
    packed[p] = (a & 15) | ((b & 15) << 4); packed[half + p] = (a >> 4) | (b & 240);
  } else if (policy == 2 && y < h) {
    packed[size_t(y) * w + x] = a; packed[size_t(y) * w + x + 1] = b;
  } else {
    if constexpr (policy == 2) p = size_t(w) * h + size_t(y - h) * (w / 2) + x / 2;
    packed[p] = quantize(a, y >= h) | (quantize(b, y >= h) << 4);
  }
}
template<int policy> __global__ void unpack_nibbles(const unsigned char *packed, unsigned char *dst, size_t pitch, int w, int h) {
  int x = 2 * (blockIdx.x * blockDim.x + threadIdx.x), y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h * 3 / 2) return;
  size_t p = size_t(y) * (w / 2) + x / 2, half = size_t(w) * h * 3 / 4;
  unsigned a, b;
  if constexpr (policy == 0) {
    a = (packed[p] & 15) | ((packed[half + p] & 15) << 4); b = (packed[p] >> 4) | (packed[half + p] & 240);
  } else if (policy == 2 && y < h) {
    a = packed[size_t(y) * w + x]; b = packed[size_t(y) * w + x + 1];
  } else {
    if constexpr (policy == 2) p = size_t(w) * h + size_t(y - h) * (w / 2) + x / 2;
    a = expand(packed[p] & 15, y >= h); b = expand(packed[p] >> 4, y >= h);
  }
  dst[size_t(y) * pitch + x] = a; dst[size_t(y) * pitch + x + 1] = b;
}
// Reversible rectangle permutation control. Preserve each UV pair's channel order.
__global__ void fold_rows(const unsigned char *src, size_t sp, unsigned char *dst, size_t dp, int w, int h) {
  int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h * 3 / 2) return;
  bool uv = y >= h; int yy = uv ? y - h : y, channels = uv ? 2 : 1;
  int sx = ((yy / (uv ? 8 : 16)) & 1) ? ((w / channels - 1 - x / channels) * channels + x % channels) : x;
  dst[size_t(y) * dp + x] = src[size_t(y) * sp + sx];
}
__device__ static float bilinear(const unsigned char *src, size_t pitch, int w, int h, int channels, int channel, float x, float y) {
  int ix = int(floorf(x)), iy = int(floorf(y)); float fx = x - ix, fy = y - iy, value = 0;
  for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i)
    value += src[size_t(max(0, min(h - 1, iy + j))) * pitch + max(0, min(w - 1, ix + i)) * channels + channel]
             * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
  return value;
}
__global__ void resample(const unsigned char *src, size_t sp, int iw, int ih, unsigned char *dst, size_t dp, int ow, int oh, const Axis *xs, const Axis *ys, bool prefilter) {
  int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= ow || y >= oh * 3 / 2) return;
  bool uv = y >= oh; int channels = uv ? 2 : 1;
  Axis ax = xs[uv ? ow + x / 2 : x], ay = ys[y];
  const auto *plane = src + (uv ? size_t(ih) * sp : 0);
  int nx = prefilter ? min(8, max(1, int(ceilf(ax.footprint)))) : 1;
  int ny = prefilter ? min(8, max(1, int(ceilf(ay.footprint)))) : 1;
  float v = 0;
  for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i)
    v += bilinear(plane, sp, iw / channels, uv ? ih / 2 : ih, channels, x % channels,
                  ax.position + ((i + .5f) / nx - .5f) * (prefilter ? ax.footprint : 0),
                  ay.position + ((j + .5f) / ny - .5f) * (prefilter ? ay.footprint : 0));
  dst[size_t(y) * dp + x] = static_cast<unsigned char>(floorf(fminf(255, fmaxf(0, v / (nx * ny))) + .5f));
}
static dim3 grid(int w, int rows) { return dim3((w + 31) / 32, (rows + 7) / 8); }
static constexpr dim3 block(32, 8);

struct Experiment {
  Image source, forward_full, restored, half;
  Bytes packed;
  Maps uniform, uniform_inverse, hyperbolic, hyperbolic_inverse;
  Timer timer;
  int w, h;
  Experiment(int w, int h) : source(w,h), forward_full(w,h), restored(w,h), half(w/2,h/2), packed(size_t(w)*h*3/2),
    uniform(w,h,w/2,h/2,0,false), uniform_inverse(w/2,h/2,w,h,0,true),
    hyperbolic(w,h,w/2,h/2,strength,false), hyperbolic_inverse(w/2,h/2,w,h,strength,true), w(w), h(h) {}
  void forward(int mode) {
    auto s = timer.stream; auto pairs = grid(w / 2, h * 3 / 2);
    if (mode == 0) check(cudaMemcpy2DAsync(forward_full.data, forward_full.pitch, source.data, source.pitch, w, h * 3 / 2, cudaMemcpyDeviceToDevice, s));
    if (mode == 1) pack_nibbles<0><<<pairs,block,0,s>>>(source.data, source.pitch, w, h, packed.data);
    if (mode == 2) pack_nibbles<1><<<pairs,block,0,s>>>(source.data, source.pitch, w, h, packed.data);
    if (mode == 3) pack_nibbles<2><<<pairs,block,0,s>>>(source.data, source.pitch, w, h, packed.data);
    if (mode == 4) fold_rows<<<grid(w,h*3/2),block,0,s>>>(source.data,source.pitch,forward_full.data,forward_full.pitch,w,h);
    if (mode >= 5) {
      const auto &maps = mode == 5 ? uniform : hyperbolic;
      resample<<<grid(w/2,h*3/4),block,0,s>>>(source.data,source.pitch,w,h,half.data,half.pitch,w/2,h/2,maps.x,maps.y,true);
    }
    check(cudaGetLastError());
  }
  void restore(int mode) {
    auto s = timer.stream; auto pairs = grid(w / 2, h * 3 / 2);
    if (mode == 1) unpack_nibbles<0><<<pairs,block,0,s>>>(packed.data,restored.data,restored.pitch,w,h);
    if (mode == 2) unpack_nibbles<1><<<pairs,block,0,s>>>(packed.data,restored.data,restored.pitch,w,h);
    if (mode == 3) unpack_nibbles<2><<<pairs,block,0,s>>>(packed.data,restored.data,restored.pitch,w,h);
    if (mode == 4) fold_rows<<<grid(w,h*3/2),block,0,s>>>(forward_full.data,forward_full.pitch,restored.data,restored.pitch,w,h);
    if (mode >= 5) {
      const auto &maps = mode == 5 ? uniform_inverse : hyperbolic_inverse;
      resample<<<grid(w,h*3/2),block,0,s>>>(half.data,half.pitch,w/2,h/2,restored.data,restored.pitch,w,h,maps.x,maps.y,false);
    }
    check(cudaGetLastError());
  }
  const Image &result(int mode) const { return mode == 0 ? forward_full : restored; }
  const Image &encoder_input(int mode) const { return mode >= 5 ? half : mode >= 1 && mode <= 3 ? restored : forward_full; }
  size_t packed_bytes(int mode) const {
    size_t n = size_t(w) * h;
    return mode == 2 ? n * 3 / 4 : mode == 3 ? n * 5 / 4 : mode >= 5 ? n * 3 / 8 : n * 3 / 2;
  }
};

static std::vector<unsigned char> synthetic(int w, int h, int frame) {
  std::vector<unsigned char> v(size_t(w) * h * 3 / 2);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    int xx = (x + frame * 7) % w, region = xx * 4 / w, value;
    if (region == 0) value = 16 + 219 * xx / std::max(1, w / 4);
    else if (region == 1) value = ((xx / 3 + y / 3) & 1) ? 210 : 35;
    else if (region == 2) value = (xx % 31 < 2 || y % 23 < 2) ? 230 : 80 + y % 70;
    else { unsigned z = unsigned(xx) * 0x45d9f3bu ^ unsigned(y) * 0x119de1f3u; z ^= z >> 16; value = 16 + z % 220; }
    v[size_t(y) * w + x] = static_cast<unsigned char>(std::clamp(value, 16, 235));
  }
  for (int y = 0; y < h / 2; ++y) for (int x = 0; x < w; x += 2) {
    v[size_t(w) * h + size_t(y) * w + x] = 16 + (x / 2 + frame * 7) * 224 / std::max(1,w / 2 - 1) % 225;
    v[size_t(w) * h + size_t(y) * w + x + 1] = 16 + y * 224 / std::max(1,h / 2 - 1);
  }
  return v;
}
// Independent scalar/double oracle. No device quantizer or device maps reused.
static unsigned cpu_quant(unsigned v, bool uv) {
  double a = std::clamp(int(v), 16, uv ? 240 : 235);
  int q = uv ? (a < 128 ? 8 - int(std::floor((128-a)/14+.5)) : 8 + int(std::floor((a-128)/16+.5)))
             : int(std::floor((a-16)*15/219+.5));
  return unsigned(uv ? (q < 8 ? 16 + 14 * q : 128 + 16 * (q - 8)) : 16 + std::floor(q * 219. / 15 + .5));
}
static double cpu_resample(const std::vector<unsigned char> &src, int iw, int ih, int ow, int oh, int x, int y, double k, bool inverse) {
  bool uv = y >= oh; int c = uv ? 2 : 1, channel = x % c;
  int ww = iw / c, hh = uv ? ih / 2 : ih, xo = x / c, yo = uv ? y - oh : y;
  auto axis = [=](int p, int in, int out) {
    double u = 2. * (p + .5) / out - 1, v = u, derivative = 1;
    if (k && !inverse) { v = std::sinh(k*u)/std::sinh(k); derivative = k*std::cosh(k*u)/std::sinh(k); }
    if (k && inverse) { v = std::asinh(u*std::sinh(k))/k; derivative = std::sinh(k)/(k*std::sqrt(1+u*u*std::sinh(k)*std::sinh(k))); }
    return std::pair<double,double>{(v+1)*in/2-.5, derivative*in/out};
  };
  auto ax = axis(xo,ww,ow/c), ay = axis(yo,hh,uv ? oh/2 : oh);
  int nx = inverse ? 1 : std::clamp(int(std::ceil(ax.second)),1,8), ny = inverse ? 1 : std::clamp(int(std::ceil(ay.second)),1,8);
  double sum = 0;
  for (int j=0;j<ny;++j) for (int i=0;i<nx;++i) {
    double sx = ax.first + (inverse ? 0 : ((i+.5)/nx-.5)*ax.second), sy = ay.first + (inverse ? 0 : ((j+.5)/ny-.5)*ay.second);
    int ix = int(std::floor(sx)), iy = int(std::floor(sy)); double fx=sx-ix, fy=sy-iy;
    for (int b=0;b<2;++b) for (int a=0;a<2;++a)
      sum += src[(uv ? size_t(iw)*ih : 0) + size_t(std::clamp(iy+b,0,hh-1))*iw + std::clamp(ix+a,0,ww-1)*c+channel] * (a?fx:1-fx)*(b?fy:1-fy);
  }
  return std::floor(sum/(nx*ny)+.5);
}
static void validate(Experiment &e, int mode, const std::vector<unsigned char> &src) {
  auto actual = e.result(mode).read(); size_t n = size_t(e.w) * e.h;
  if ((mode == 0 || mode == 1 || mode == 4) && actual != src)
    throw std::runtime_error(modes[mode]+" lossless roundtrip mismatch at "+std::to_string(e.w)+"x"+std::to_string(e.h));
  if (mode == 4) {
    auto folded=e.forward_full.read();
    for(int y=0;y<e.h*3/2;++y) for(int x=0;x<e.w;++x) {
      bool uv=y>=e.h; int row=uv?y-e.h:y, channels=uv?2:1;
      int sx=(row/(uv?8:16))%2 ? (e.w/channels-1-x/channels)*channels+x%channels : x;
      require(folded[size_t(y)*e.w+x]==src[size_t(y)*e.w+sx],"row fold CPU oracle mismatch");
    }
  }
  if (mode == 2 || mode == 3) for (size_t i=0;i<actual.size();++i)
    require(actual[i] == ((mode==3 && i<n) ? src[i] : cpu_quant(src[i],i>=n)), "quantizer CPU oracle mismatch");
  if (mode >= 1 && mode <= 3) {
    auto packed = e.packed.read(e.packed_bytes(mode));
    for (size_t i=0;i<src.size();i+=2) {
      size_t p=i/2; unsigned expected;
      if (mode==1) {
        require(packed[p] == ((src[i]&15)|((src[i+1]&15)<<4)), "low nibble order mismatch");
        require(packed[src.size()/2+p] == ((src[i]>>4)|(src[i+1]&240)), "high nibble order mismatch");
      } else if (mode==3 && i<n) require(packed[i]==src[i] && packed[i+1]==src[i+1], "luma was changed");
      else {
        if (mode==3) p=n+(i-n)/2;
        // Compare expanded nibbles independently, including neutral chroma.
        unsigned qa=packed[p]&15, qb=packed[p]>>4;
        auto dec=[=](unsigned q) { return i>=n ? (q<8 ? 16+14*q : 128+16*(q-8)) : 16+unsigned(std::floor(q*219./15+.5)); };
        expected=cpu_quant(src[i],i>=n); require(dec(qa)==expected && dec(qb)==cpu_quant(src[i+1],i>=n), "packed quantizer mismatch");
      }
    }
  }
  if (mode >= 5) {
    auto fwd=e.half.read(); double k=mode==5 ? 0 : strength;
    auto compare=[&](const std::vector<unsigned char>&a,const std::vector<unsigned char>&b,int iw,int ih,int ow,int oh,bool inverse) {
      int step=ow<100 ? 1 : 37;
      for (int y=0;y<oh*3/2;y+=step) for(int x=0;x<ow;x+=step)
        require(std::abs(int(b[size_t(y)*ow+x])-cpu_resample(a,iw,ih,ow,oh,x,y,k,inverse))<=1, "warp CPU oracle mismatch");
    };
    compare(src,fwd,e.w,e.h,e.w/2,e.h/2,false); compare(fwd,actual,e.w/2,e.h/2,e.w,e.h,true);
  }
}
static void quality(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b, int w, int h) {
  double sq=0, abs=0, center=0, outer=0, uv=0; size_t nc=0,no=0; int maxerr=0;
  for (int y=0;y<h;++y) for(int x=0;x<w;++x) {
    int d=std::abs(int(a[size_t(y)*w+x])-b[size_t(y)*w+x]); abs+=d; sq+=d*d; maxerr=std::max(maxerr,d);
    if (x>=w*3/8 && x<w*5/8 && y>=h*3/8 && y<h*5/8) { center+=d; ++nc; } else { outer+=d; ++no; }
  }
  for(size_t i=size_t(w)*h;i<a.size();++i) uv+=std::abs(int(a[i])-b[i]);
  std::printf("\"y_mae\":%.6f,\"y_max_error\":%d,\"center_y_mae\":%.6f,\"outer_y_mae\":%.6f,\"uv_mae\":%.6f,\"y_psnr_db\":",abs/(w*h),maxerr,center/nc,outer/no,uv/(size_t(w)*h/2));
  if(sq) std::printf("%.6f",10*std::log10(255.*255*w*h/sq)); else std::printf("null");
}
static void quantiles(const char *name,std::vector<double> v) {
  std::sort(v.begin(),v.end()); std::printf(",\"%s_us\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}",name,v[v.size()/2],v[v.size()*95/100],v[v.size()*99/100]);
}
static void self_test() {
  // Pitched/tail rows, tiny chroma planes, all 256x256 byte pairs, legal endpoints.
  for(auto size:{std::pair{4,4},std::pair{12,8},std::pair{68,20},std::pair{1024,256}}) {
    Experiment e(size.first,size.second); auto v=synthetic(e.w,e.h,0);
    if(e.w==1024) for(size_t i=0;i<v.size();i+=2) { v[i]=(i/2)&255; v[i+1]=((i/2)>>8)&255; }
    e.source.upload(v);
    for(int mode=0;mode<int(modes.size());++mode) {
      // Each mode gets a fresh guard, including the unused part of chroma4.
      check(cudaMemsetAsync(e.packed.data,0xcd,e.packed.n+64,e.timer.stream));
      e.forward(mode); e.restore(mode); check(cudaStreamSynchronize(e.timer.stream)); validate(e,mode,v);
    }
  }
  std::puts("{\"self_test\":\"pass\",\"byte_pair_cases_per_plane\":65536,\"padding_and_guards\":true,\"cpu_warp_tolerance_lsb\":1}");
}
static void write(FILE *f,const std::vector<unsigned char>&v) { require(std::fwrite(v.data(),1,v.size(),f)==v.size(),"fixture write failed"); }
static void generate(const std::filesystem::path &dir) {
  require(std::filesystem::is_directory(dir),"fixture directory must already exist");
  Experiment e(width,height); auto src=synthetic(width,height,0); e.source.upload(src);
  for(int round=0;round<2;++round) for(int order=0;order<int(modes.size());++order) {
    int mode=round ? int(modes.size())-1-order : order; std::vector<double> forward,restore,gpu,wall;
    check(cudaMemsetAsync(e.packed.data,0xcd,e.packed.n+64,e.timer.stream));
    for(int i=0;i<90;++i) {
      auto start=std::chrono::steady_clock::now();
      check(cudaEventRecord(e.timer.begin,e.timer.stream)); e.forward(mode);
      check(cudaEventRecord(e.timer.middle,e.timer.stream)); e.restore(mode);
      check(cudaEventRecord(e.timer.end,e.timer.stream)); check(cudaEventSynchronize(e.timer.end));
      auto stop=std::chrono::steady_clock::now(); float a,b,c;
      check(cudaEventElapsedTime(&a,e.timer.begin,e.timer.middle)); check(cudaEventElapsedTime(&b,e.timer.middle,e.timer.end)); check(cudaEventElapsedTime(&c,e.timer.begin,e.timer.end));
      if(i>=10) {forward.push_back(a*1000.);restore.push_back(b*1000.);gpu.push_back(c*1000.);wall.push_back(std::chrono::duration<double,std::micro>(stop-start).count());}
    }
    validate(e,mode,src);
    std::printf("{\"mode\":\"%s\",\"round\":%d,\"samples\":80,\"source_bytes\":%zu,\"intermediate_bytes\":%zu,\"encoder_width\":%d,\"encoder_height\":%d,",modes[mode].c_str(),round,src.size(),e.packed_bytes(mode),mode>=5?width/2:width,mode>=5?height/2:height);
    quality(src,e.result(mode).read(),width,height);
    quantiles("forward_gpu",forward);quantiles("restore_gpu",restore);quantiles("roundtrip_gpu",gpu);quantiles("roundtrip_cpu_wall",wall);std::puts("}");
  }
  for(int mode=0;mode<int(modes.size());++mode) {
    File file(std::fopen((dir/(modes[mode]+".nv12")).c_str(),"wb")); require(bool(file),"cannot create fixture");
    for(int frame=0;frame<fixture_frames;++frame) {
      e.source.upload(synthetic(width,height,frame));e.forward(mode);e.restore(mode);check(cudaStreamSynchronize(e.timer.stream));
      write(file.get(),e.encoder_input(mode).read());
    }
  }
}
static std::vector<unsigned char> read_file(const char *path) {
  auto n=std::filesystem::file_size(path); require(n<=size_t(width)*height*3/2*fixture_frames,"fixture too large");
  std::vector<unsigned char> v(n); File file(std::fopen(path,"rb"));require(bool(file),"cannot open fixture");
  require(std::fread(v.data(),1,v.size(),file.get())==v.size(),"fixture read failed");return v;
}
static void decoded_quality(int mode,const char *decoded_path,const char *original_path) {
  Experiment e(width,height);auto decoded=read_file(decoded_path),original=read_file(original_path);
  size_t framebytes=size_t(mode>=5?width/2:width)*(mode>=5?height/2:height)*3/2, fullbytes=size_t(width)*height*3/2;
  require(decoded.size()==2*framebytes && original.size()==fixture_frames*fullbytes,"expected first/last decoded frames and four source frames");
  for(int f=0;f<2;++f) {
    std::vector<unsigned char> input(decoded.begin()+f*framebytes,decoded.begin()+(f+1)*framebytes),actual;
    if(mode>=4) {
      if(mode>=5)e.half.upload(input);else e.forward_full.upload(input);
      e.restore(mode);check(cudaStreamSynchronize(e.timer.stream));actual=e.restored.read();
    }else actual=input; // Nibbles were expanded on the host before hardware encoding.
    std::vector<unsigned char> reference(original.begin()+f*fullbytes,original.begin()+(f+1)*fullbytes);
    std::printf("{\"mode\":\"%s\",\"decoded_frame\":%d,",modes[mode].c_str(),f?49:0);quality(reference,actual,width,height);std::puts("}");
  }
}
#ifdef SUNSHINE_RESIDUAL_PROBE
#include "pixel_residual.cuh"
#ifdef SUNSHINE_SPARSE_PROBE
#include "pixel_sparse.cuh"
#endif
#endif
int main(int argc,char **argv) {
  try {
#ifdef SUNSHINE_RESIDUAL_PROBE
#ifdef SUNSHINE_SPARSE_PROBE
    if(argc==2 && std::string(argv[1])=="--sparse-self-test")sparse_pixels::self_test();
    else if(argc==2 && std::string(argv[1])=="--sparse-bench")sparse_pixels::benchmark();
    else if(argc==2 && std::string(argv[1])=="--sparse-estimated-bench"){sparse_pixels::estimate_from_pixels=true;sparse_pixels::benchmark();}
    else if(argc==2 && std::string(argv[1])=="--sparse-mean-bench"){sparse_pixels::estimate_from_pixels=true;sparse_pixels::use_mean_bias=true;sparse_pixels::benchmark();}
    else
#endif
    if(argc==2 && std::string(argv[1])=="--residual-self-test")residual::self_test();
    else if(argc==2 && std::string(argv[1])=="--residual-bench")residual::benchmark();
    else if(argc==2 && std::string(argv[1])=="--residual-rle-bench"){residual::compress_modes=true;residual::benchmark();}
    else
#endif
    if(argc==2 && std::string(argv[1])=="--self-test")self_test();
    else if(argc==3 && std::string(argv[1])=="--generate") {self_test();generate(argv[2]);}
    else if(argc==5 && std::string(argv[1])=="--decoded-quality") {
      auto p=std::find(modes.begin(),modes.end(),argv[2]);require(p!=modes.end(),"unknown mode");decoded_quality(int(p-modes.begin()),argv[3],argv[4]);
    }else throw std::runtime_error("usage: surface_packing --self-test | --generate EXISTING_DIR | --decoded-quality MODE DECODED ORIGINAL");
    return 0;
  }catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
