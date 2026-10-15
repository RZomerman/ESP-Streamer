#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t interval;
  uint32_t audio_remaining;
  size_t metadata_remaining;
  size_t metadata_length;
  bool awaiting_length;
  char block[4081];
} icy_metadata_t;

void icy_metadata_init(icy_metadata_t *parser, uint32_t interval);
size_t icy_metadata_filter(icy_metadata_t *parser, uint8_t *data, size_t length,
                           bool *updated, char *title, size_t title_size);
