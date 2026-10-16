#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline bool dit4096_rate_supported(uint32_t rate) {
  return rate == 32000 || rate == 44100 || rate == 48000;
}

// I2S 24-bit words occupy the upper 24 bits of each 32-bit DMA word.
static inline uint32_t dit4096_pack_sample(int16_t sample) {
  return (uint32_t)(uint16_t)sample << 16;
}

static inline uint32_t dit4096_mclk(uint32_t rate) { return rate * 256; }
static inline uint32_t dit4096_bclk(uint32_t rate) { return rate * 64; }
