// SPDX-License-Identifier: GPL-3.0-only
/** @file gpu_conversion.cu
 * @brief Validate and time a fused SDR BT.709 RGB-to-NV12 conversion prototype.
 * CUDA-only experiment: does not emulate GL/CUDA fences, KMS import, or the TV.
 */
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

/** @brief Check a CUDA result. */
static void check(cudaError_t status) {
  if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}
/** @brief Own one device allocation during tests and exceptions. */
struct Buffer {
  unsigned char *p = nullptr; ///< Device pointer.
  size_t pitch = 0; ///< Byte stride.
  Buffer(size_t width, size_t height) { check(cudaMallocPitch(&p, &pitch, width, height)); }
  ~Buffer() { cudaFree(p); }
  Buffer(const Buffer &) = delete;
};
/** @brief Own reusable timing events and a private nonblocking stream. */
struct Timing {
  cudaEvent_t a = nullptr, b = nullptr; ///< GPU timestamps.
  cudaStream_t stream = nullptr; ///< Isolated work stream.
  Timing() {
    try {
      check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
      check(cudaEventCreate(&a)); check(cudaEventCreate(&b));
    } catch (...) {
      if (a) cudaEventDestroy(a);
      if (b) cudaEventDestroy(b);
      if (stream) cudaStreamDestroy(stream);
      throw;
    }
  }
  ~Timing() { if (a) cudaEventDestroy(a); if (b) cudaEventDestroy(b); if (stream) cudaStreamDestroy(stream); }
};
/** @brief Sample scaled BGRA8 using bilinear filtering with edge clamping. */
__device__ float3 sample(const unsigned char *source, size_t stride, int iw, int ih, int ow, int oh, int x, int y) {
  float sx = (x + .5f) * iw / ow - .5f, sy = (y + .5f) * ih / oh - .5f;
  int x0 = int(floorf(sx)), y0 = int(floorf(sy));
  float fx = sx - x0, fy = sy - y0;
  float3 result = make_float3(0, 0, 0);
  for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
    int xx = max(0, min(iw - 1, x0 + i)), yy = max(0, min(ih - 1, y0 + j));
    uchar4 p = reinterpret_cast<const uchar4 *>(source + size_t(yy) * stride)[xx];
    float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
    result.x += p.z * w; result.y += p.y * w; result.z += p.x * w;
  }
  return result;
}
/** @brief Round/clamp one limited-range 8-bit component. */
__device__ unsigned char pack(float value, float lo, float hi) {
  return static_cast<unsigned char>(floorf(fminf(hi, fmaxf(lo, value)) + .5f));
}
/** @brief Compute BT.709 luma in the input's 0..255 domain. */
__device__ float luma(float3 p) { return .2126f * p.x + .7152f * p.y + .0722f * p.z; }
/** @brief Separate luma pass for the reference representation. */
__global__ void y_pass(const unsigned char *src, size_t sp, int iw, int ih, unsigned char *dst, size_t dp, int ow, int oh) {
  int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= ow || y >= oh) return;
  dst[size_t(y) * dp + x] = pack(16 + luma(sample(src, sp, iw, ih, ow, oh, x, y)) * (219.f / 255.f), 16, 235);
}
/** @brief Shared chroma conversion and optional fused luma writes for each 2x2 tile. */
template<bool fused>
__global__ void uv_pass(const unsigned char *src, size_t sp, int iw, int ih, unsigned char *ydst, size_t yp,
                        unsigned char *uvdst, size_t up, int ow, int oh) {
  int x = 2 * (blockIdx.x * blockDim.x + threadIdx.x), y = 2 * (blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= ow || y >= oh) return;
  float3 mean = make_float3(0, 0, 0);
  for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
    auto p = sample(src, sp, iw, ih, ow, oh, x + i, y + j);
    mean.x += p.x * .25f; mean.y += p.y * .25f; mean.z += p.z * .25f;
    if (fused) ydst[size_t(y + j) * yp + x + i] = pack(16 + luma(p) * (219.f / 255.f), 16, 235);
  }
  float yy = luma(mean);
  auto *uv = uvdst + size_t(y / 2) * up + x;
  uv[0] = pack(128 + (mean.z - yy) * (224.f / (255.f * 1.8556f)), 16, 240);
  uv[1] = pack(128 + (mean.x - yy) * (224.f / (255.f * 1.5748f)), 16, 240);
}
/** @brief Print CPU/GPU latency quantiles in microseconds. */
static void print(const char *key, std::vector<double> v) {
  std::sort(v.begin(), v.end());
  std::printf(",\"%s_us\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}", key, v[v.size() / 2], v[v.size() * 95 / 100], v[v.size() * 99 / 100]);
}
/** @brief Exercise one input/output size; compare both GPU paths and a double-precision CPU reference. */
static void run(int iw, int ih, int ow, int oh) {
  Buffer input(size_t(iw) * 4, ih), temp_y(ow, oh), temp_uv(ow, oh / 2), out_y(ow, oh), out_uv(ow, oh / 2);
  Timing timer;
  std::vector<unsigned char> pixels(size_t(iw) * ih * 4);
  for (int y = 0; y < ih; ++y) for (int x = 0; x < iw; ++x) {
    auto *p = pixels.data() + (size_t(y) * iw + x) * 4;
    p[0] = (x * 17 + y * 19) % 256; p[1] = (x * 7 + y * 3) % 256; p[2] = (x * 13 + y * 29) % 256; p[3] = 255;
    if (x < iw / 8) p[0] = p[1] = p[2] = 0;
    if (x >= iw * 7 / 8) p[0] = p[1] = p[2] = 255;
  }
  check(cudaMemcpy2D(input.p, input.pitch, pixels.data(), size_t(iw) * 4, size_t(iw) * 4, ih, cudaMemcpyHostToDevice));
  dim3 block(32, 8), full((ow + 31) / 32, (oh + 7) / 8), half((ow / 2 + 31) / 32, (oh / 2 + 7) / 8);
  std::vector<unsigned char> reference;
  for (int round = 0; round < 2; ++round) for (int mode = 0; mode < 2; ++mode) {
    bool fused = mode == round;
    std::vector<double> gpu, cpu;
    for (int iteration = 0; iteration < 220; ++iteration) {
      auto a = std::chrono::steady_clock::now();
      check(cudaEventRecord(timer.a, timer.stream));
      if (fused) {
        uv_pass<true><<<half, block, 0, timer.stream>>>(input.p, input.pitch, iw, ih, out_y.p, out_y.pitch, out_uv.p, out_uv.pitch, ow, oh);
      } else {
        y_pass<<<full, block, 0, timer.stream>>>(input.p, input.pitch, iw, ih, temp_y.p, temp_y.pitch, ow, oh);
        uv_pass<false><<<half, block, 0, timer.stream>>>(input.p, input.pitch, iw, ih, nullptr, 0, temp_uv.p, temp_uv.pitch, ow, oh);
        check(cudaMemcpy2DAsync(out_y.p, out_y.pitch, temp_y.p, temp_y.pitch, ow, oh, cudaMemcpyDeviceToDevice, timer.stream));
        check(cudaMemcpy2DAsync(out_uv.p, out_uv.pitch, temp_uv.p, temp_uv.pitch, ow, oh / 2, cudaMemcpyDeviceToDevice, timer.stream));
      }
      check(cudaGetLastError()); check(cudaEventRecord(timer.b, timer.stream));
      check(cudaEventSynchronize(timer.b)); // Measurement boundary only, never proposed for production.
      auto b = std::chrono::steady_clock::now();
      float ms;
      check(cudaEventElapsedTime(&ms, timer.a, timer.b));
      if (iteration >= 20) {
        gpu.push_back(ms * 1000.); cpu.push_back(std::chrono::duration<double, std::micro>(b - a).count());
      }
    }
    std::vector<unsigned char> actual(size_t(ow) * oh * 3 / 2);
    check(cudaMemcpy2D(actual.data(), ow, out_y.p, out_y.pitch, ow, oh, cudaMemcpyDeviceToHost));
    check(cudaMemcpy2D(actual.data() + size_t(ow) * oh, ow, out_uv.p, out_uv.pitch, ow, oh / 2, cudaMemcpyDeviceToHost));
    if (reference.empty()) reference = actual;
    if (reference != actual) throw std::runtime_error("fused/reference GPU paths disagree");
    // Independent double-precision CPU reference, sampled across tiles including black/white and image edges.
    auto sample_cpu = [&](int x, int y, int channel) {
      double sx = (x + .5) * iw / ow - .5, sy = (y + .5) * ih / oh - .5;
      int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
      double fx = sx - x0, fy = sy - y0, result = 0;
      for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i)
        result += pixels[(size_t(std::clamp(y0 + j, 0, ih - 1)) * iw + std::clamp(x0 + i, 0, iw - 1)) * 4 + channel] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
      return result;
    };
    auto compare = [&](size_t index, double expected) {
      if (std::abs(int(actual[index]) - int(std::floor(expected + .5))) > 1) throw std::runtime_error("CPU color reference mismatch");
    };
    for (int y = 0; y < oh; y += std::max(1, oh / 32) * 2) for (int x = 0; x < ow; x += std::max(1, ow / 32) * 2) {
      double mean[3] = {};
      for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
        double b = sample_cpu(x + i, y + j, 0), g = sample_cpu(x + i, y + j, 1), r = sample_cpu(x + i, y + j, 2);
        compare(size_t(y + j) * ow + x + i, 16 + (.2126 * r + .7152 * g + .0722 * b) * 219 / 255);
        mean[0] += r * .25; mean[1] += g * .25; mean[2] += b * .25;
      }
      double yy = .2126 * mean[0] + .7152 * mean[1] + .0722 * mean[2];
      compare(size_t(ow) * oh + size_t(y / 2) * ow + x, 128 + (mean[2] - yy) * 224 / (255 * 1.8556));
      compare(size_t(ow) * oh + size_t(y / 2) * ow + x + 1, 128 + (mean[0] - yy) * 224 / (255 * 1.5748));
    }
    std::printf("{\"input_width\":%d,\"input_height\":%d,\"width\":%d,\"height\":%d,\"fused\":%s,\"round\":%d,\"samples\":200,\"gpu_byte_equal\":true,\"cpu_reference_tolerance\":1", iw, ih, ow, oh, fused ? "true" : "false", round);
    print("gpu", gpu); print("cpu_wall", cpu); std::puts("}");
  }
}
/** @brief Reserve SM execution for a bounded interval using the hardware nanosecond timer. */
__global__ void contend(unsigned long long duration_ns) {
  unsigned long long start, now;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(start));
  do {
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(now));
  } while (now - start < duration_ns);
}
/** @brief Provide explicitly synthetic, bounded cross-context GPU interference. */
static void load(int seconds) {
  Timing timing;
  cudaDeviceProp properties;
  check(cudaGetDeviceProperties(&properties, 0));
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  bool announced = false;
  while (std::chrono::steady_clock::now() < deadline) {
    contend<<<properties.multiProcessorCount * 2, 256, 0, timing.stream>>>(4000000);
    check(cudaGetLastError()); check(cudaStreamSynchronize(timing.stream));
    if (!announced) { std::puts("Synthetic GPU contention ready"); std::fflush(stdout); announced = true; }
  }
}
/** @brief Run bounded synthetic conversion comparisons on the current CUDA GPU. */
int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--contention-seconds") {
      char *end;
      long seconds = std::strtol(argv[2], &end, 10);
      if (*end || seconds < 1 || seconds > 180) throw std::runtime_error("contention duration must be 1..180 seconds");
      load(int(seconds)); return 0;
    }
    if (argc != 1) throw std::runtime_error("usage: gpu_conversion [--contention-seconds 1..180]");
    run(1920, 1080, 1920, 1080);
    run(3840, 2160, 1920, 1080);
    run(3840, 2160, 3840, 2160);
    return 0;
  } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
