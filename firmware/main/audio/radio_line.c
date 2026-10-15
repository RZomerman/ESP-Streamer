#include "radio_line.h"

int radio_line_feed(radio_line_t *line, unsigned char byte) {
  if (byte == '\n') {
    bool invalid = line->invalid;
    line->data[line->length] = 0;
    bool empty = line->length == 0;
    line->length = 0;
    line->invalid = false;
    return invalid ? -1 : empty ? 0 : 1;
  }
  if (line->invalid) return 0;
  if (!byte || line->length == RADIO_LINE_BYTES - 1) {
    line->invalid = true;
    return 0;
  }
  line->data[line->length++] = (char)byte;
  return 0;
}
