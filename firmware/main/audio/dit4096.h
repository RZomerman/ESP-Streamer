#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool enabled;
  bool running;
  uint32_t sample_rate;
  uint32_t write_errors;
  uint32_t queue_overflows;
  uint32_t starvation_resets;
  uint32_t frames_submitted;
} dit4096_status_t;

esp_err_t dit4096_init(void);
void dit4096_flush(void);
void dit4096_submit(const int16_t *pcm, size_t frames, uint32_t rate, int32_t gain_q15);
void dit4096_get_status(dit4096_status_t *status);
