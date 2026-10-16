#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool airplay;
  int64_t connected_us;
} led_playback_t;

static inline bool led_playback_update(led_playback_t *state, bool playing,
                                       bool airplay, int64_t now_us) {
  if (airplay && !state->airplay) state->connected_us = now_us;
  state->airplay = airplay;
  if (airplay) return ((now_us - state->connected_us) / 1000000) % 2 == 0;
  return playing;
}
