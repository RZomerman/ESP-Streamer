#include "../main/audio/icy_metadata.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void fragmented(size_t chunk_size) {
  const char metadata[] = "StreamTitle='Artist - It's a song';";
  uint8_t wire[256];
  size_t length = 0;
  memcpy(wire + length, "ABCDE", 5);
  length += 5;
  size_t padded = (strlen(metadata) + 15) & ~(size_t)15;
  wire[length++] = (uint8_t)(padded / 16);
  memset(wire + length, 0, padded);
  memcpy(wire + length, metadata, strlen(metadata));
  length += padded;
  memcpy(wire + length, "FGHIJ", 5);
  length += 5;
  wire[length++] = 0;
  memcpy(wire + length, "KLMNO", 5);
  length += 5;
  wire[length++] = 1;
  memset(wire + length, 0, 16);
  memcpy(wire + length, "StreamTitle='';", 15);
  length += 16;
  memcpy(wire + length, "PQ", 2);
  length += 2;

  icy_metadata_t parser;
  icy_metadata_init(&parser, 5);
  char title[128] = {0}, audio[128] = {0};
  size_t audio_length = 0;
  unsigned updates = 0;
  for (size_t offset = 0; offset < length;) {
    size_t n = length - offset;
    if (n > chunk_size) n = chunk_size;
    uint8_t chunk[256];
    memcpy(chunk, wire + offset, n);
    bool updated;
    size_t emitted = icy_metadata_filter(&parser, chunk, n, &updated, title, sizeof(title));
    memcpy(audio + audio_length, chunk, emitted);
    audio_length += emitted;
    if (updated) {
      // A chunk can contain both updates; the last one must win.
      assert(!strcmp(title, "Artist - It's a song") || !strcmp(title, ""));
      updates++;
    }
    offset += n;
  }
  assert(audio_length == 17);
  assert(!memcmp(audio, "ABCDEFGHIJKLMNOPQ", 17));
  assert(updates > 0);
  assert(!strcmp(title, ""));
}

int main(void) {
  for (size_t n = 1; n <= 128; ++n) fragmented(n);

  icy_metadata_t parser;
  uint8_t block[4087] = {'a', 'b', 'c', 255};
  memset(block + 4, 'x', 4080);
  memcpy(block + 4, "StreamTitle='Maximum block';", 28);
  memcpy(block + 4084, "def", 3);
  icy_metadata_init(&parser, 3);
  bool updated;
  char title[128];
  assert(icy_metadata_filter(&parser, block, sizeof(block), &updated, title, sizeof(title)) == 6);
  assert(!memcmp(block, "abcdef", 6));
  assert(updated && !strcmp(title, "Maximum block"));

  uint8_t plain[] = {1, 2, 3, 0, 255};
  icy_metadata_init(&parser, 0);
  assert(icy_metadata_filter(&parser, plain, sizeof(plain), &updated, title, sizeof(title)) == sizeof(plain));
  assert(!updated && plain[4] == 255);

  uint8_t malformed[18] = {'A', 1};
  memset(malformed + 2, 'x', 16);
  icy_metadata_init(&parser, 1);
  strcpy(title, "Previous");
  assert(icy_metadata_filter(&parser, malformed, sizeof(malformed), &updated, title, sizeof(title)) == 1);
  assert(!updated && !strcmp(title, "Previous"));
  puts("ICY tests passed: 128 fragmentation sizes, empty blocks/titles, apostrophes, maximum block, no metadata and missing title");
}
