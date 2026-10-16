#include "../main/led_playback.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  led_playback_t state = {0};
  assert(!led_playback_update(&state, false, false, 0));
  assert(led_playback_update(&state, true, false, 100));
  assert(led_playback_update(&state, true, false, 3000000));
  assert(!led_playback_update(&state, false, false, 4000000));

  const int64_t connected = 5000000;
  assert(led_playback_update(&state, false, true, connected));
  assert(led_playback_update(&state, true, true, connected + 999999));
  assert(!led_playback_update(&state, true, true, connected + 1000000));
  assert(!led_playback_update(&state, false, true, connected + 1999999));
  assert(led_playback_update(&state, false, true, connected + 2000000));
  assert(!led_playback_update(&state, false, true, connected + 3000000));
  assert(led_playback_update(&state, true, true, connected + 4000000));
  assert(led_playback_update(&state, false, true, connected + 1000000000000LL));

  assert(!led_playback_update(&state, false, false, connected + 1000000000001LL));
  assert(led_playback_update(&state, true, false, connected + 1000000000002LL));
  assert(led_playback_update(&state, false, true, connected + 1000000000003LL));
  assert(!led_playback_update(&state, false, true, connected + 1000001000003LL));
  assert(!led_playback_update(&state, false, false, connected + 1000001000004LL));
  puts("LED playback tests passed: idle, radio, AirPlay pause/resume, 1s edges and reconnect");
  return 0;
}
