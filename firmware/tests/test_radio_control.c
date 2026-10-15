#include "../main/audio/radio_command.h"
#include "../main/audio/radio_line.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool parse(const char *text, radio_command_t *command) {
  cJSON *json = radio_json_parse(text, strlen(text));
  bool valid = radio_command_parse(json, command);
  cJSON_Delete(json);
  return valid;
}

static void commands(void) {
  radio_command_t command;
  for (int i = 0; i < 5; ++i) {
    char text[100];
    snprintf(text, sizeof(text), "{\"station\":%d%s}", i,
             i == 4 ? ",\"url\":\"http://example.org/live\"" : "");
    assert(parse(text, &command));
    assert(command.station == i && !command.override);
  }
  assert(parse("{\"url\":\"https://example.org:443/live?a=1&b=2\",\"override\":true}", &command));
  assert(command.station == 4 && command.override);
  assert(parse("{\"station\":0,\"override\":false}", &command) && !command.override);
  const char *invalid[] = {
      "null", "[]", "{}", "{\"station\":true}", "{\"station\":\"0\"}",
      "{\"station\":-1}", "{\"station\":5}", "{\"station\":1.2}",
      "{\"station\":4}", "{\"station\":0,\"url\":\"https://example.org\"}",
      "{\"url\":\"http:///missing-host\"}", "{\"url\":\"ftp://example.org\"}",
      "{\"url\":\"http://\"}", "{\"url\":\"http://user:pass@example.org/\"}",
      "{\"url\":\"https://example.org/has space\"}", "{\"url\":\"https://example.org/#fragment\"}",
      "{\"url\":\"https://example.org/\\\\bad\"}", "{\"url\":\"https://example.org/\\u0000bad\"}",
      "{\"station\":0,\"override\":1}", "{\"station\":0,\"override\":\"true\"}",
      "{\"station\":0} trailing", "{\"station\":0}{\"station\":1}",
      "[[[[[[[[[]]]]]]]]]"
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    assert(!parse(invalid[i], &command));
  char url[540];
  memset(url, 'x', sizeof(url));
  memcpy(url, "{\"url\":\"http://", 15);
  memcpy(url + sizeof(url) - 3, "\"}", 3);
  assert(!parse(url, &command));
}

static void lines(void) {
  const char *text = "{\"cmd\":\"set_stream\",\"url\":\"http://example.org/live\",\"override\":true}\r\n";
  size_t length = strlen(text);
  for (size_t fragment = 1; fragment <= length; ++fragment) {
    radio_line_t line = {0};
    int complete = 0;
    for (size_t offset = 0; offset < length; offset += fragment) {
      size_t end = offset + fragment < length ? offset + fragment : length;
      for (size_t i = offset; i < end; ++i) {
        int result = radio_line_feed(&line, (unsigned char)text[i]);
        if (result) {
          assert(result == 1);
          cJSON *json = radio_json_parse(line.data, strlen(line.data));
          radio_command_t command;
          assert(radio_command_parse(json, &command));
          assert(command.station == 4 && command.override);
          cJSON_Delete(json);
          complete++;
        }
      }
    }
    assert(complete == 1 && line.length == 0);
  }
  radio_line_t line = {0};
  for (int i = 0; i < RADIO_LINE_BYTES - 1; ++i)
    assert(radio_line_feed(&line, 'x') == 0);
  assert(radio_line_feed(&line, '\n') == 1);
  for (int i = 0; i < RADIO_LINE_BYTES + 100; ++i)
    assert(radio_line_feed(&line, 'x') == 0);
  assert(radio_line_feed(&line, '\n') == -1);
  assert(radio_line_feed(&line, '\n') == 0);
  assert(radio_line_feed(&line, 0) == 0);
  assert(radio_line_feed(&line, 'a') == 0);
  assert(radio_line_feed(&line, '\n') == -1);
  assert(radio_line_feed(&line, '{') == 0);
  assert(radio_line_feed(&line, '}') == 0);
  assert(radio_line_feed(&line, '\n') == 1);
  assert(!strcmp(line.data, "{}"));
}

int main(void) {
  commands();
  lines();
  puts("Radio control tests passed: command validation, overrides, JSON bounds, fragmented CRLF, overflow and resynchronization");
  return 0;
}
