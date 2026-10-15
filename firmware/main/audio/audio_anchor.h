#pragma once

#include <stdint.h>

#ifdef __cplusplus
static constexpr
#else
static inline
#endif
uint32_t audio_anchor_project_rtp(uint32_t rtp, uint64_t from_ns,
                                  uint64_t to_ns, uint32_t sample_rate) {
  int64_t delta_ns = (int64_t)(to_ns - from_ns);
  // Split seconds from the remainder to avoid overflowing the ns * rate product.
  int64_t samples = (delta_ns / 1000000000LL) * sample_rate +
                    ((delta_ns % 1000000000LL) * sample_rate) / 1000000000LL;
  return rtp + (uint32_t)samples;
}
