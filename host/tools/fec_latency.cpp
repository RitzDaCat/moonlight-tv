// SPDX-License-Identifier: GPL-3.0-only
/** @file fec_latency.cpp
 * @brief Compare fresh and cached FEC matrix setup, including actual parity work.
 */
#include <src/fec_encoder_cache.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
#include <cstring>
#include <random>
#include <atomic>
#include <string_view>
#if __has_include("src/fec_gfni.h")
#include "src/fec_gfni.h"
namespace fec_tiled = stream::fec::gfni;
constexpr bool production_tiled_header = true;
#else
namespace fec_tiled {
inline bool available() { return false; }
inline int encode(reed_solomon *rs, unsigned char **shards, int count, int bytes) {
  return reed_solomon_encode(rs, shards, count, bytes);
}
}
constexpr bool production_tiled_header = false;
#endif

/** @brief Make every output observable to the compiler without adding timed machine work. */
static inline void observe(void *p) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(p) : "memory");
#else
  (void)p;
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
/** @brief Measure one shard shape with a fixed, preallocated payload. */
static void run(int data, int parity, bool cached, bool encode, bool tiled = false, int round = 0) {
  constexpr int block = 1344;
  stream::fec::rs_cache_t cache;
  std::unique_ptr<unsigned char, decltype(&reed_solomon_free)> storage {
    static_cast<unsigned char *>(reed_solomon_aligned_alloc(size_t(data + parity) * block)), reed_solomon_free};
  if (!storage) throw std::bad_alloc();
  std::vector<unsigned char *> shards(data + parity);
  for (int i = 0; i < data + parity; ++i) {
    shards[i] = storage.get() + i * block;
    for (int b = 0; b < block; ++b) shards[i][b] = static_cast<unsigned char>(i * 13 + b * 17);
  }
  std::vector<double> values;
  for (int i = 0; i < 1200; ++i) {
    shards[(i * 7) % data][(i * 17) % block] ^= static_cast<unsigned char>(i + 1);
    observe(shards.data());
    auto a = std::chrono::steady_clock::now();
    {
      std::unique_ptr<reed_solomon, stream::fec::rs_deleter_t> fresh;
      auto *rs = cached ? cache.get(data, parity) : (fresh.reset(reed_solomon_new(data, parity)), fresh.get());
      if (!rs) throw std::bad_alloc();
      observe(shards.data());
      if (encode && (tiled ? fec_tiled::encode(rs, shards.data(), data + parity, block) : reed_solomon_encode(rs, shards.data(), data + parity, block))) throw std::runtime_error("FEC encode failed");
      observe(shards.data());
    }
    auto b = std::chrono::steady_clock::now();
    if (i >= 200) values.push_back(std::chrono::duration<double, std::nano>(b - a).count());
  }
  std::sort(values.begin(), values.end());
  std::printf("{\"tiled_gfni\":%s,\"round\":%d,\"data\":%d,\"parity\":%d,\"block\":%d,\"cached\":%s,\"includes_parity\":%s,\"p50_ns\":%.1f,\"p95_ns\":%.1f,\"p99_ns\":%.1f,\"check\":%u}\n",
              tiled ? "true" : "false", round, data, parity, block, cached ? "true" : "false", encode ? "true" : "false", values[500], values[950], values[990], shards[data][0]);
}
/** @brief Independent polynomial multiply using the nanors 0x11d field. */
static unsigned char scalar_product(unsigned a, unsigned b) {
  unsigned out = 0;
  for (unsigned bit = 0; bit < 8; ++bit) {
    if (b & 1) out ^= a;
    b >>= 1; a <<= 1;
    if (a & 256) a ^= 0x11d;
  }
  return static_cast<unsigned char>(out);
}
/** @brief Verify every field product, exact byte tails, untouched guards and erasure recovery. */
static void validate_tiled() {
  auto require = [](bool ok, const char *why) { if (!ok) throw std::runtime_error(why); };
  require(fec_tiled::encode(nullptr, nullptr, 0, 1) < 0, "Invalid matrix accepted");
  std::unique_ptr<reed_solomon, stream::fec::rs_deleter_t> one {reed_solomon_new(1, 1)};
  unsigned char input[256], output[256];
  for (unsigned i = 0; i < 256; ++i) input[i] = i;
  unsigned char *pair[] {input, output};
  require(fec_tiled::encode(one.get(), pair, 1, 256) < 0, "Short shard array accepted");
  require(fec_tiled::encode(one.get(), pair, 2, 0) < 0, "Zero block accepted");
  for (unsigned coefficient = 0; coefficient < 256; ++coefficient) {
    one->p[0] = coefficient;
    require(fec_tiled::encode(one.get(), pair, 2, 256) == 0, "Field test encode failed");
    for (unsigned x = 0; x < 256; ++x) require(output[x] == scalar_product(coefficient, x), "GFNI field mapping differs");
  }
  std::mt19937 random(0x40901344);
  unsigned cases = 0, recoveries = 0;
  for (auto [data, parity] : {std::pair{1, 1}, {8, 2}, {32, 7}, {128, 26}, {200, 40}, {127, 128}, {254, 1}, {1, 254}}) {
    std::unique_ptr<reed_solomon, stream::fec::rs_deleter_t> rs {reed_solomon_new(data, parity)};
    require(bool(rs), "Matrix allocation failed");
    for (int bytes : {1, 3, 15, 31, 63, 64, 65, 127, 255, 256, 257, 1344, 1408, 9001}) {
      constexpr int guard = 67;
      std::vector<std::vector<unsigned char>> original(data + parity), tiled(data + parity);
      std::vector<unsigned char *> ref(data + parity), fast(data + parity);
      for (int i = 0; i < data + parity; ++i) {
        original[i].assign(bytes + guard * 2 + 3, 0xcd);
        for (int j = 0; j < bytes; ++j) original[i][guard + i % 4 + j] = random() & 255;
        tiled[i] = original[i]; ref[i] = original[i].data() + guard + i % 4; fast[i] = tiled[i].data() + guard + i % 4;
      }
      require(reed_solomon_encode(rs.get(), ref.data(), data + parity, bytes) == 0, "Reference parity failed");
      require(fec_tiled::encode(rs.get(), fast.data(), data + parity, bytes) == 0, "Tiled parity failed");
      require(original == tiled, "Parity, input bytes or guards differ");
      auto saved = tiled;
      std::vector<unsigned char> erased(data + parity, 0);
      // Lose data and optionally parity while preserving enough independent shards.
      const int lost = std::min(data, std::max(1, parity / 2));
      for (int i = 0; i < lost; ++i) { erased[i] = 1; std::memset(fast[i], 0, bytes); }
      if (lost < parity) { erased.back() = 1; std::memset(fast.back(), 0, bytes); }
      require(reed_solomon_decode(rs.get(), fast.data(), erased.data(), data + parity, bytes) == 0, "Parity recovery failed");
      for (int i = 0; i < data; ++i) require(tiled[i] == saved[i], "Recovered data or guards differ");
      ++recoveries; ++cases;
    }
  }
  std::printf("{\"validation\":\"pass\",\"gfni_available\":%s,\"field_products\":65536,\"guarded_cases\":%u,\"recovery_cases\":%u}\n", fec_tiled::available() ? "true" : "false", cases, recoveries);
}
/** @brief Run both setup-only and complete-FEC comparisons in reversed order. */
int main(int argc, char **argv) {
  try {
  reed_solomon_init();
  bool tiled = argc == 2 && std::string_view(argv[1]) == "--tiled";
  if (argc != 1 && !tiled) throw std::runtime_error("Usage: fec_latency [--tiled]");
  if (tiled) {
    if (!production_tiled_header) throw std::runtime_error("Apply patch0010 before the tiled comparison");
    validate_tiled();
    for (int round = 0; round < 2; ++round)
      for (auto shape : {std::pair{8, 2}, std::pair{32, 7}, std::pair{128, 26}, std::pair{200, 40}})
        for (int step = 0; step < 2; ++step) run(shape.first, shape.second, true, true, step == round, round);
    return 0;
  }
  for (int round = 0; round < 2; ++round)
    for (auto shape : {std::pair{8, 2}, std::pair{32, 7}, std::pair{128, 26}, std::pair{200, 40}})
      for (bool encode : {false, true})
        for (int order = 0; order < 2; ++order) run(shape.first, shape.second, order == round, encode, false, round);
  return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
