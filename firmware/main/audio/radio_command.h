#pragma once

#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  int station;
  const char *url;
  bool override;
} radio_command_t;

bool radio_command_parse(const cJSON *json, radio_command_t *command);
cJSON *radio_json_parse(const char *data, size_t size);
