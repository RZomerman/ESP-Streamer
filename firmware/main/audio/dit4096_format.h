#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline bool dit4096_rate_supported(uint32_t rate) {
  return rate == 32000 || rate == 44100 || rate == 48000;
}

// The 24-bit sample sits in the upper bits of a 32-bit I2S word; the DIT4096 is
// strapped for 24-bit I2S and ignores the zero low byte. 32-bit channel width keeps
// the ESP-IDF clock dividers exact (24-bit width skews them).
static inline uint32_t dit4096_pack_sample(int16_t sample) {
  return (uint32_t)(uint16_t)sample << 16;
}

static inline uint32_t dit4096_mclk(uint32_t rate) { return rate * 256; }
static inline uint32_t dit4096_bclk(uint32_t rate) { return rate * 64; }
