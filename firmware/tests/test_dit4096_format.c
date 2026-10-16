#include "../main/audio/dit4096_format.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  for (int sample = INT16_MIN; sample <= INT16_MAX; ++sample) {
    uint32_t word = dit4096_pack_sample((int16_t)sample);
    assert((word & 0xffffu) == 0);
    assert((int16_t)(word >> 16) == sample);
    assert((word >> 8) == ((uint32_t)(uint16_t)sample << 8));
  }
  const uint32_t rates[] = {32000, 44100, 48000};
  const uint32_t mclk[] = {8192000, 11289600, 12288000};
  const uint32_t bclk[] = {2048000, 2822400, 3072000};
  for (unsigned i = 0; i < 3; ++i) {
    assert(dit4096_rate_supported(rates[i]));
    assert(dit4096_mclk(rates[i]) == mclk[i]);
    assert(dit4096_bclk(rates[i]) == bclk[i]);
    assert(dit4096_mclk(rates[i]) / dit4096_bclk(rates[i]) == 4);
  }
  assert(!dit4096_rate_supported(0));
  assert(!dit4096_rate_supported(22050));
  assert(!dit4096_rate_supported(88200));
  assert(!dit4096_rate_supported(96000));
  puts("DIT4096 tests passed: all 65536 signed samples, native rates and clock ratios");
  return 0;
}
