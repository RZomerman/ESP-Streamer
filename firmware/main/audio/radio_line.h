#pragma once
#include <stdbool.h>
#include <stddef.h>

#define RADIO_LINE_BYTES 1024

typedef struct {
  char data[RADIO_LINE_BYTES];
  size_t length;
  bool invalid;
} radio_line_t;

// 0: incomplete, 1: complete, -1: discard invalid/oversized line at newline.
int radio_line_feed(radio_line_t *line, unsigned char byte);
