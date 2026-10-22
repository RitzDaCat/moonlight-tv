// SPDX-License-Identifier: GPL-3.0-only
/** @file nvenc_latency.cpp
 * @brief Offline NVENC service-time probe. No capture, network, decoder or display.
 */
#include <cuda.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static const char *output_root = nullptr; ///< Explicit opt-in directory for decoder-validation fixtures.
static int io_stream_mode = 1; ///< 0=legacy, 1=host-style blocking, 2=nonblocking, 3=highest stream priority.
static const char *fixture_source = nullptr; ///< Optional four-frame, tightly packed NV12 input; never used by default.

/** @brief Close an optional synthetic stream fixture. */
struct FileCloser {
  void operator()(FILE *file) const { std::fclose(file); }
};

/** @brief Check an FFmpeg operation. */
static void check(int rc) {
  if (rc < 0) {
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(rc, message, sizeof(message));
    throw std::runtime_error(message);
  }
}
/** @brief Check a CUDA operation. */
static void gpu(CUresult rc) {
  if (rc != CUDA_SUCCESS) {
    const char *message = nullptr;
    cuGetErrorString(rc, &message);
    throw std::runtime_error(message ? message : "CUDA error");
  }
}
/** @brief Return elapsed CPU wall time in microseconds. */
static double us(std::chrono::steady_clock::time_point a,
                 std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::micro>(b - a).count();
}
/** @brief Print sorted latency quantiles. */
static void quantiles(const char *name, std::vector<double> values) {
  std::sort(values.begin(), values.end());
  auto q = [&](double p) { return values[std::min(values.size() - 1, size_t(p * values.size()))]; };
  std::printf(",\"%s_us\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}", name, q(.5), q(.95), q(.99));
}
/** @brief Own FFmpeg/CUDA resources, including exceptions during initialization. */
struct Resources {
  AVBufferRef *device = nullptr; ///< CUDA device.
  AVBufferRef *frames = nullptr; ///< Hardware frame pool.
  AVCodecContext *codec = nullptr; ///< Encoder.
  AVFrame *frame = nullptr; ///< Input frame.
  AVPacket *packet = nullptr; ///< Output packet.
  bool pushed = false; ///< CUDA context is current.
  CUstream stream = nullptr; ///< Explicit I/O stream, when selected.
  ~Resources() {
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    av_buffer_unref(&frames);
    if (stream) cuStreamDestroy(stream);
    if (pushed) {
      CUcontext previous;
      cuCtxPopCurrent(&previous);
    }
    av_buffer_unref(&device);
  }
};
/** @brief Measure one codec/multipass/resolution combination using a moving NV12 pattern. */
static void run(const char *codec_name, int width, int height, const char *multipass, const char *split) {
  Resources r;
  AVDictionary *device_options = nullptr;
  // Match the host's private FFmpeg CUDA context rather than the runtime primary context.
  int rc = av_hwdevice_ctx_create(&r.device, AV_HWDEVICE_TYPE_CUDA, "0", device_options, 0);
  av_dict_free(&device_options);
  check(rc);
  auto *cuda = static_cast<AVCUDADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(r.device->data)->hwctx);
  gpu(cuCtxPushCurrent(cuda->cuda_ctx));
  r.pushed = true;
  if (io_stream_mode) {
    if (io_stream_mode == 3) {
      int least, greatest;
      gpu(cuCtxGetStreamPriorityRange(&least, &greatest));
      gpu(cuStreamCreateWithPriority(&r.stream, CU_STREAM_NON_BLOCKING, greatest));
    } else {
      gpu(cuStreamCreate(&r.stream, io_stream_mode == 2 ? CU_STREAM_NON_BLOCKING : CU_STREAM_DEFAULT));
    }
    cuda->stream = r.stream;
  }
  r.frames = av_hwframe_ctx_alloc(r.device);
  if (!r.frames) throw std::bad_alloc();
  auto *pool = reinterpret_cast<AVHWFramesContext *>(r.frames->data);
  pool->format = AV_PIX_FMT_CUDA;
  pool->sw_format = AV_PIX_FMT_NV12;
  pool->width = width;
  pool->height = height;
  check(av_hwframe_ctx_init(r.frames));
  const AVCodec *encoder = avcodec_find_encoder_by_name(codec_name);
  if (!encoder) throw std::runtime_error("encoder unavailable");
  r.codec = avcodec_alloc_context3(encoder);
  r.frame = av_frame_alloc();
  r.packet = av_packet_alloc();
  if (!r.codec || !r.frame || !r.packet) throw std::bad_alloc();
  r.codec->width = width;
  r.codec->height = height;
  r.codec->pix_fmt = AV_PIX_FMT_CUDA;
  r.codec->time_base = AVRational{1, 120};
  r.codec->framerate = AVRational{120, 1};
  r.codec->bit_rate = r.codec->rc_max_rate = r.codec->rc_min_rate = 23000000;
  r.codec->rc_buffer_size = 23000000 / 120;
  r.codec->max_b_frames = 0;
  r.codec->gop_size = 2147483647;
  r.codec->hw_frames_ctx = av_buffer_ref(r.frames);
  AVDictionary *options = nullptr;
  av_dict_set(&options, "preset", "p1", 0);
  av_dict_set(&options, "tune", "ull", 0);
  av_dict_set(&options, "rc", "cbr", 0);
  av_dict_set(&options, "multipass", multipass, 0);
  av_dict_set(&options, "zerolatency", "1", 0);
  av_dict_set(&options, "surfaces", "1", 0);
  av_dict_set(&options, "delay", "0", 0);
  av_dict_set(&options, "rc-lookahead", "0", 0);
  av_dict_set(&options, "split_encode_mode", split, 0);
  rc = avcodec_open2(r.codec, encoder, &options);
  bool unused = av_dict_count(options) != 0;
  av_dict_free(&options);
  check(rc);
  if (unused) throw std::runtime_error("unused encoder option");
  check(av_hwframe_get_buffer(r.frames, r.frame, 0));
  std::unique_ptr<FILE, FileCloser> stream_file;
  if (output_root) {
    auto name = std::string(codec_name) + "-" + std::to_string(width) + "x" + std::to_string(height) + "-" + multipass + "-" + split +
                (r.codec->codec_id == AV_CODEC_ID_HEVC ? ".hevc" : ".obu");
    stream_file.reset(std::fopen((std::filesystem::path(output_root) / name).c_str(), "wb"));
    if (!stream_file) throw std::runtime_error("cannot open requested stream output");
  }
  std::vector<unsigned char> fixture;
  if (fixture_source) {
    size_t expected = size_t(width) * height * 3 / 2 * 4;
    if (std::filesystem::file_size(fixture_source) != expected)
      throw std::runtime_error("fixture must contain exactly four tightly packed NV12 frames");
    fixture.resize(expected);
    std::unique_ptr<FILE, FileCloser> file(std::fopen(fixture_source, "rb"));
    if (!file || std::fread(fixture.data(), 1, fixture.size(), file.get()) != fixture.size())
      throw std::runtime_error("cannot read fixture");
  }
  std::vector<unsigned char> source(size_t(width) * height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      source[size_t(y) * width + x] = 16 + ((x * 13 + y * 17 + ((x ^ y) >> 2)) % 220);
  CUDA_MEMCPY2D copy{};
  copy.srcMemoryType = CU_MEMORYTYPE_HOST;
  copy.srcHost = source.data();
  copy.srcPitch = width;
  copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  copy.dstDevice = reinterpret_cast<CUdeviceptr>(r.frame->data[0]);
  copy.dstPitch = r.frame->linesize[0];
  copy.WidthInBytes = width;
  copy.Height = height;
  gpu(cuMemcpy2D(&copy));
  gpu(cuMemsetD2D8(reinterpret_cast<CUdeviceptr>(r.frame->data[1]), r.frame->linesize[1], 128, width, height / 2));
  std::vector<double> service, send, receive, ready, update_encode;
  size_t bytes = 0;
  const int warmup = fixture_source ? 10 : 20, samples = fixture_source ? 40 : 100;
  for (int i = 0; i < warmup + samples; ++i) {
    // Separate input readiness from encode-only service: CUDA memset may return asynchronously.
    auto update_start = std::chrono::steady_clock::now();
    if (fixture_source) {
      // Upload/readiness deliberately excluded from encode-only service timing.
      copy.srcHost = fixture.data() + size_t(i % 4) * width * height * 3 / 2;
      copy.dstDevice = reinterpret_cast<CUdeviceptr>(r.frame->data[0]);
      copy.dstPitch = r.frame->linesize[0]; copy.Height = height;
      gpu(cuMemcpy2D(&copy));
      copy.srcHost = static_cast<const unsigned char *>(copy.srcHost) + size_t(width) * height;
      copy.dstDevice = reinterpret_cast<CUdeviceptr>(r.frame->data[1]);
      copy.dstPitch = r.frame->linesize[1]; copy.Height = height / 2;
      gpu(cuMemcpy2D(&copy));
    } else {
      gpu(cuMemsetD2D8(reinterpret_cast<CUdeviceptr>(r.frame->data[0]) + size_t(i * 7 % (height - 64)) * r.frame->linesize[0],
                      r.frame->linesize[0], 16 + i % 220, width, 64));
    }
    gpu(cuStreamSynchronize(nullptr)); // Fence the legacy-stream source update before encoder timing.
    auto update_end = std::chrono::steady_clock::now();
    r.frame->pts = i;
    auto a = std::chrono::steady_clock::now();
    check(avcodec_send_frame(r.codec, r.frame));
    auto b = std::chrono::steady_clock::now();
    rc = avcodec_receive_packet(r.codec, r.packet);
    auto c = std::chrono::steady_clock::now();
    check(rc); // EAGAIN is not silently counted as a completed frame.
    if (r.packet->pts != i) throw std::runtime_error("delayed or mismatched encoder output");
    if (stream_file && std::fwrite(r.packet->data, 1, r.packet->size, stream_file.get()) != size_t(r.packet->size))
      throw std::runtime_error("stream output write failed");
    if (i >= warmup) {
      service.push_back(us(a, c)); send.push_back(us(a, b)); receive.push_back(us(b, c));
      ready.push_back(us(update_start, update_end)); update_encode.push_back(us(update_start, c));
      bytes += r.packet->size;
    }
    av_packet_unref(r.packet);
  }
  std::printf("{\"codec\":\"%s\",\"width\":%d,\"height\":%d,\"fps\":120,\"multipass\":\"%s\",\"split\":\"%s\",\"samples\":%d,\"bytes\":%zu,\"source_readiness_fenced\":true,\"cuda_context\":\"private\",\"io_stream_mode\":%d",
              codec_name, width, height, multipass, split, samples, bytes, io_stream_mode);
  std::printf(",\"fixture_source_frames\":%d,\"warmup\":%d", fixture_source ? 4 : 0, warmup);
  quantiles("service", service); quantiles("send", send); quantiles("receive", receive);
  quantiles("source_update", ready); quantiles("update_plus_encode", update_encode);
  std::puts("}");
}
/** @brief Run bounded synthetic configurations, without contacting a streaming client. */
int main(int argc, char **argv) {
  try {
    bool contention_probe = argc == 2 && std::string(argv[1]) == "--contention-probe";
    bool stream_probe = argc == 2 && std::string(argv[1]) == "--stream-probe";
    bool fixture_probe = argc == 6 && std::string(argv[1]) == "--fixture-probe";
    if (argc == 3 && std::string(argv[1]) == "--write-streams") output_root = argv[2];
    else if (argc != 1 && !contention_probe && !stream_probe && !fixture_probe) throw std::runtime_error("usage: nvenc_latency [--write-streams EXISTING_DIRECTORY | --contention-probe | --stream-probe | --fixture-probe FOUR_FRAME_NV12 WIDTH HEIGHT EXISTING_OUTPUT_DIR]");
    av_log_set_level(AV_LOG_ERROR);
    gpu(cuInit(0));
    std::fprintf(stderr, "Synthetic ready-GPU-frame encode only; FFmpeg %s; excludes capture/conversion/network/display\n", av_version_info());
    if (fixture_probe) {
      auto dimension = [](const char *s) {
        char *end = nullptr; long v = std::strtol(s, &end, 10);
        if (end == s || *end || v < 4 || v > 8192 || v % 2) throw std::runtime_error("fixture dimensions must be even and 4..8192");
        return int(v);
      };
      fixture_source = argv[2]; output_root = argv[5];
      run("hevc_nvenc", dimension(argv[3]), dimension(argv[4]), "disabled", "forced");
      return 0;
    }
    if (stream_probe) {
      for (const char *codec : {"hevc_nvenc", "av1_nvenc"})
        for (int mode : {0, 1, 2, 3}) {
          io_stream_mode = mode;
          run(codec, 1920, 1080, "disabled", "forced");
        }
      return 0;
    }
    if (contention_probe) {
      for (const char *codec : {"hevc_nvenc", "av1_nvenc"})
        for (int order : {0, 1, 1, 0})
          run(codec, 1920, 1080, order ? "disabled" : "qres", order ? "forced" : "auto");
      return 0;
    }
    for (int round = 0; round < 2; ++round)
      for (const char *codec : {"hevc_nvenc", "av1_nvenc"})
        for (auto size : {std::pair{1920, 1080}, std::pair{3840, 2160}})
          for (const char *pass : {"qres", "disabled"})
            for (int i = 0; i < 2; ++i)
              run(codec, size.first, size.second, pass, i == round ? "auto" : "forced");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
