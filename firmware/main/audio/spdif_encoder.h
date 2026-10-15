#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  unsigned frame;
  bool level;
} spdif_encoder_t;

#ifdef __cplusplus
#define SPDIF_CONSTEXPR constexpr
#else
#define SPDIF_CONSTEXPR
#endif

static inline SPDIF_CONSTEXPR uint64_t spdif_subframe(spdif_encoder_t *encoder, int16_t sample,
                                      bool right) {
  uint8_t preamble = right ? 0xe4 : (encoder->frame == 0 ? 0xe8 : 0xe2);
  if (encoder->level) preamble ^= 0xff;
  uint64_t symbols = preamble;
  encoder->level = preamble & 1;
  bool status = encoder->frame == 2 || encoder->frame == 25 ||
                encoder->frame == 33 ||
                (encoder->frame == 20 && !right) ||
                (encoder->frame == 21 && right);
  uint32_t payload = ((uint32_t)(uint16_t)sample << 12) | ((uint32_t)status << 30);
  unsigned parity = 0;
  for (unsigned bit = 4; bit <= 30; ++bit) parity ^= (payload >> bit) & 1;
  payload |= (uint32_t)parity << 31;
  for (unsigned bit = 4; bit <= 31; ++bit) {
    encoder->level = !encoder->level;
    symbols = (symbols << 1) | encoder->level;
    if ((payload >> bit) & 1) encoder->level = !encoder->level;
    symbols = (symbols << 1) | encoder->level;
  }
  return symbols;
}

static inline SPDIF_CONSTEXPR void spdif_encode(spdif_encoder_t *encoder, int16_t left,
                                 int16_t right, uint32_t words[4]) {
  uint64_t l = spdif_subframe(encoder, left, false);
  uint64_t r = spdif_subframe(encoder, right, true);
  words[0] = l >> 32;
  words[1] = l;
  words[2] = r >> 32;
  words[3] = r;
  encoder->frame = (encoder->frame + 1) % 192;
}

#undef SPDIF_CONSTEXPR
