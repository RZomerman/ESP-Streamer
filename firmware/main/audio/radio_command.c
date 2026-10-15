#include "radio_command.h"
#include <string.h>

cJSON *radio_json_parse(const char *data, size_t size) {
  int depth = 0;
  bool string = false, escape = false;
  if (!size || memchr(data, 0, size)) return NULL;
  for (size_t i = 0; i < size; ++i) {
    char c = data[i];
    if (string) {
      if (escape) {
        if (c == 'u' && i + 4 < size && !memcmp(data + i + 1, "0000", 4))
          return NULL;
        escape = false;
      } else if (c == '\\') escape = true;
      else if (c == '"') string = false;
    } else if (c == '"') string = true;
    else if (c == '[' || c == '{') {
      if (++depth > 8) return NULL;
    } else if (c == ']' || c == '}') {
      if (--depth < 0) return NULL;
    }
  }
  if (depth || string) return NULL;
  return cJSON_ParseWithLengthOpts(data, size + 1, NULL, true);
}

static bool valid_url(const char *url) {
  if (!url || strlen(url) >= 512) return false;
  const char *host = !strncmp(url, "http://", 7) ? url + 7 :
                     (!strncmp(url, "https://", 8) ? url + 8 : NULL);
  if (!host || !*host || *host == '/') return false;
  for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
    if (*p <= 32 || *p >= 127 || *p == '\\' || *p == '#' || *p == '@') return false;
  return true;
}

bool radio_command_parse(const cJSON *json, radio_command_t *command) {
  if (!cJSON_IsObject(json)) return false;
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "station");
  const cJSON *url = cJSON_GetObjectItemCaseSensitive(json, "url");
  const cJSON *override = cJSON_GetObjectItemCaseSensitive(json, "override");
  if (override && !cJSON_IsBool(override)) return false;
  if (!id && cJSON_IsString(url)) command->station = 4;
  else if (cJSON_IsNumber(id) && id->valuedouble >= 0 && id->valuedouble <= 4 &&
           id->valuedouble == id->valueint) command->station = id->valueint;
  else return false;
  if (command->station == 4 && (!cJSON_IsString(url) || !valid_url(url->valuestring)))
    return false;
  if (command->station != 4 && url) return false;
  command->url = command->station == 4 ? url->valuestring : NULL;
  command->override = cJSON_IsTrue(override);
  return true;
}
