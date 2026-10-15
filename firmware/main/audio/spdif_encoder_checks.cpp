#include "spdif_encoder.h"

namespace {
constexpr bool verify(uint64_t symbols, uint8_t preamble, bool &level,
                      int16_t expected, bool status) {
  if (uint8_t(symbols >> 56) != uint8_t(level ? preamble ^ 0xff : preamble))
    return false;
  level = (symbols >> 56) & 1;
  uint32_t payload = 0;
  unsigned parity = 0;
  for (unsigned bit = 4; bit <= 31; ++bit) {
    unsigned shift = 54 - (bit - 4) * 2;
    bool first = (symbols >> (shift + 1)) & 1;
    bool second = (symbols >> shift) & 1;
    if (first == level) return false;
    bool value = first != second;
    payload |= uint32_t(value) << bit;
    parity ^= value;
    level = second;
  }
  return parity == 0 && ((payload >> 28) & 3) == 0 &&
         uint16_t(payload >> 12) == uint16_t(expected) &&
         bool((payload >> 30) & 1) == status;
}

constexpr bool blocks() {
  spdif_encoder_t encoder = {};
  bool level = false;
  for (unsigned frame = 0; frame < 384; ++frame) {
    int16_t left = int16_t(uint16_t(frame * 257));
    int16_t right = int16_t(uint16_t(~uint16_t(frame * 193)));
    uint32_t words[4] = {};
    spdif_encode(&encoder, left, right, words);
    unsigned index = frame % 192;
    bool common_status = index == 2 || index == 25 || index == 33;
    if (!verify((uint64_t(words[0]) << 32) | words[1],
                index == 0 ? 0xe8 : 0xe2, level, left, common_status || index == 20) ||
        !verify((uint64_t(words[2]) << 32) | words[3],
                0xe4, level, right, common_status || index == 21)) return false;
  }
  return encoder.frame == 0;
}

constexpr bool extremes() {
  spdif_encoder_t encoder = {};
  bool level = false;
  constexpr int16_t samples[] = {0, 1, -1, 32767, -32768};
  for (unsigned i = 0; i < 5; ++i) {
    uint32_t words[4] = {};
    spdif_encode(&encoder, samples[i], samples[4 - i], words);
    if (!verify((uint64_t(words[0]) << 32) | words[1],
                i == 0 ? 0xe8 : 0xe2, level, samples[i], i == 2) ||
        !verify((uint64_t(words[2]) << 32) | words[3],
                0xe4, level, samples[4 - i], i == 2)) return false;
  }
  return true;
}
static_assert(blocks(), "S/PDIF BMC, block, parity, channel status or word order failure");
static_assert(extremes(), "S/PDIF PCM extreme sample failure");
}
