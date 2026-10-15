#include "icy_metadata.h"

#include <string.h>

void icy_metadata_init(icy_metadata_t *parser, uint32_t interval) {
  memset(parser, 0, sizeof(*parser));
  parser->interval = interval;
  parser->audio_remaining = interval;
}

static bool read_title(const char *block, char *title, size_t capacity) {
  const char *start = strstr(block, "StreamTitle='");
  if (!start || !capacity) return false;
  start += strlen("StreamTitle='");
  const char *end = strstr(start, "';");
  if (!end) return false;
  size_t length = (size_t)(end - start);
  if (length >= capacity) length = capacity - 1;
  memcpy(title, start, length);
  title[length] = 0;
  return true;
}

size_t icy_metadata_filter(icy_metadata_t *parser, uint8_t *data, size_t length,
                           bool *updated, char *title, size_t title_size) {
  *updated = false;
  if (!parser->interval) return length;
  size_t input = 0, output = 0;
  while (input < length) {
    if (parser->metadata_remaining) {
      size_t count = length - input;
      if (count > parser->metadata_remaining) count = parser->metadata_remaining;
      memcpy(parser->block + parser->metadata_length, data + input, count);
      parser->metadata_length += count;
      parser->metadata_remaining -= count;
      input += count;
      if (!parser->metadata_remaining) {
        parser->block[parser->metadata_length] = 0;
        if (read_title(parser->block, title, title_size)) *updated = true;
        parser->audio_remaining = parser->interval;
      }
    } else if (parser->awaiting_length) {
      parser->metadata_remaining = (size_t)data[input++] * 16;
      parser->metadata_length = 0;
      parser->awaiting_length = false;
      if (!parser->metadata_remaining) parser->audio_remaining = parser->interval;
    } else {
      size_t count = length - input;
      if (count > parser->audio_remaining) count = parser->audio_remaining;
      memmove(data + output, data + input, count);
      input += count;
      output += count;
      parser->audio_remaining -= count;
      if (!parser->audio_remaining) parser->awaiting_length = true;
    }
  }
  return output;
}
