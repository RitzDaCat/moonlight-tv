// SPDX-License-Identifier: GPL-3.0-only
/** @file nvenc_api_trace.cpp
 * @brief Benchmark-only NVENC function-table timing shim for one serial encoder session.
 * Load through a private LD_LIBRARY_PATH for the probe process only. Link with
 * -Wl,-z,nodelete so repeated FFmpeg dlclose/dlopen cycles retain this shim ahead
 * of the real driver's identical SONAME in the process link map.
 */
#include <ffnvcodec/nvEncodeAPI.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
NV_ENCODE_API_FUNCTION_LIST real_api {}; ///< Real function pointers, never wrappers.
void *library = nullptr; ///< Real driver library retained for the probe's lifetime.
/** @brief Bounded per-call wall-time observations, reset at encoder initialization. */
struct Samples {
  std::array<double, 1024> us {}; ///< Recent samples.
  unsigned count = 0; ///< Number observed in this serial session.
};
std::array<Samples, 6> samples; ///< Map, encode, lock, unlock, unmap, register.
unsigned width = 0, height = 0, split = 0, multipass = 0; ///< Requested configuration.
const char *codec = "unknown"; ///< Requested codec.
/** @brief Resolve the actual driver by absolute path, bypassing this private shim. */
void *driver() {
  if (!library) {
    const char *path = std::getenv("NVENC_REAL_LIBRARY");
    library = dlopen(path ? path : "/usr/lib/libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
  }
  return library;
}
/** @brief Time one real driver API call with no per-frame allocation or I/O. */
template<class F> NVENCSTATUS timed(unsigned stage, F &&fn) {
  auto a = std::chrono::steady_clock::now();
  auto status = fn();
  auto b = std::chrono::steady_clock::now();
  auto &s = samples[stage];
  s.us[s.count++ % s.us.size()] = std::chrono::duration<double, std::micro>(b - a).count();
  return status;
}
/** @brief Emit stage quantiles only after encoder teardown. */
void report() {
  const char *names[] = {"map_input", "encode_picture", "lock_bitstream", "unlock_bitstream", "unmap_input", "register_input"};
  std::fprintf(stderr, "NVENC_STAGE {\"codec\":\"%s\",\"width\":%u,\"height\":%u,\"requested_split\":%u,\"multipass\":%u", codec, width, height, split, multipass);
  for (unsigned i = 0; i < samples.size(); ++i) {
    auto &s = samples[i];
    auto n = std::min<size_t>(s.count, s.us.size());
    if (!n) continue;
    std::sort(s.us.begin(), s.us.begin() + n);
    std::fprintf(stderr, ",\"%s_us\":{\"n\":%u,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f}", names[i], s.count, s.us[n / 2], s.us[n * 95 / 100], s.us[n * 99 / 100]);
  }
  std::fputs("}\n", stderr);
}
}

/** @brief Forward driver API-version discovery. */
extern "C" NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t *version) {
  auto handle = driver();
  auto fn = handle ? reinterpret_cast<decltype(&NvEncodeAPIGetMaxSupportedVersion)>(dlsym(handle, "NvEncodeAPIGetMaxSupportedVersion")) : nullptr;
  return fn ? fn(version) : NV_ENC_ERR_GENERIC;
}
/** @brief Wrap selected entry points supplied to the standalone benchmark. */
extern "C" NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST *functions) {
  auto handle = driver();
  auto fn = handle ? reinterpret_cast<decltype(&NvEncodeAPICreateInstance)>(dlsym(handle, "NvEncodeAPICreateInstance")) : nullptr;
  if (!fn) return NV_ENC_ERR_GENERIC;
  auto status = fn(functions);
  if (status != NV_ENC_SUCCESS) return status;
  real_api = *functions;
  functions->nvEncInitializeEncoder = [](void *encoder, NV_ENC_INITIALIZE_PARAMS *params) {
    samples = {};
    width = params->encodeWidth; height = params->encodeHeight; split = params->splitEncodeMode;
    multipass = params->encodeConfig ? params->encodeConfig->rcParams.multiPass : 0;
    codec = std::memcmp(&params->encodeGUID, &NV_ENC_CODEC_AV1_GUID, sizeof(GUID)) == 0 ? "av1" : "hevc";
    return real_api.nvEncInitializeEncoder(encoder, params);
  };
  functions->nvEncSetIOCudaStreams = [](void *encoder, NV_ENC_CUSTREAM_PTR input, NV_ENC_CUSTREAM_PTR output) {
    auto result = real_api.nvEncSetIOCudaStreams(encoder, input, output);
    // The API takes pointers to CUstream handles; inspect only this owned process.
    const bool input_null = !input || !*static_cast<void **>(input);
    const bool output_null = !output || !*static_cast<void **>(output);
    std::fprintf(stderr, "NVENC_STREAM {\"input_stream_null\":%s,\"output_stream_null\":%s,\"same_stream\":%s,\"status\":%d}\n",
                 input_null ? "true" : "false", output_null ? "true" : "false",
                 (!input_null && !output_null && *static_cast<void **>(input) == *static_cast<void **>(output)) ? "true" : "false", int(result));
    return result;
  };
  functions->nvEncMapInputResource = [](void *e, NV_ENC_MAP_INPUT_RESOURCE *p) {
    return timed(0, [&] { return real_api.nvEncMapInputResource(e, p); });
  };
  functions->nvEncEncodePicture = [](void *e, NV_ENC_PIC_PARAMS *p) {
    return timed(1, [&] { return real_api.nvEncEncodePicture(e, p); });
  };
  functions->nvEncLockBitstream = [](void *e, NV_ENC_LOCK_BITSTREAM *p) {
    return timed(2, [&] { return real_api.nvEncLockBitstream(e, p); });
  };
  functions->nvEncUnlockBitstream = [](void *e, NV_ENC_OUTPUT_PTR p) {
    return timed(3, [&] { return real_api.nvEncUnlockBitstream(e, p); });
  };
  functions->nvEncUnmapInputResource = [](void *e, NV_ENC_INPUT_PTR p) {
    return timed(4, [&] { return real_api.nvEncUnmapInputResource(e, p); });
  };
  functions->nvEncRegisterResource = [](void *e, NV_ENC_REGISTER_RESOURCE *p) {
    return timed(5, [&] { return real_api.nvEncRegisterResource(e, p); });
  };
  functions->nvEncDestroyEncoder = [](void *encoder) {
    auto result = real_api.nvEncDestroyEncoder(encoder);
    report();
    return result;
  };
  return status;
}
