#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t radio_init(void);
bool radio_airplay_active(void);
uint32_t radio_generation(void);
size_t radio_read(int16_t *pcm, size_t frames, uint32_t *rate);
esp_err_t radio_register_http(httpd_handle_t server);
