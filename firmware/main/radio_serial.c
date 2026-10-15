#include "radio_serial.h"

#ifdef CONFIG_RADIO_V2
#include "radio.h"
#include "radio_command.h"
#include "radio_line.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "radio_serial";

typedef struct {
  uart_port_t port;
  int rx;
  int tx;
  const char *name;
  QueueHandle_t events;
  radio_line_t line;
} serial_link_t;

static serial_link_t links[] = {
    {.port = UART_NUM_1, .rx = 4, .tx = 5, .name = "ESP_Controller"},
    {.port = UART_NUM_2, .rx = 7, .tx = 6, .name = "ESP_DISPLAY"}};

static void send_message(serial_link_t *link, cJSON *message) {
  char *json = message ? cJSON_PrintUnformatted(message) : NULL;
  cJSON_Delete(message);
  if (!json) {
    ESP_LOGE(TAG, "%s: cannot allocate JSON response", link->name);
    const char failure[] = "{\"type\":\"error\",\"code\":500,\"message\":\"Out of memory\"}\n";
    if (uart_write_bytes(link->port, failure, sizeof(failure) - 1) < 0)
      ESP_LOGE(TAG, "%s: cannot send allocation error", link->name);
    return;
  }
  size_t length = strlen(json);
  if (uart_write_bytes(link->port, json, length) != (int)length ||
      uart_write_bytes(link->port, "\n", 1) != 1)
    ESP_LOGE(TAG, "%s: UART write failed", link->name);
  free(json);
}

static cJSON *envelope(serial_link_t *link, const char *type) {
  cJSON *message = cJSON_CreateObject();
  if (message) {
    cJSON_AddStringToObject(message, "type", type);
    cJSON_AddNumberToObject(message, "protocol_version", 2);
    cJSON_AddStringToObject(message, "link", link->name);
  }
  return message;
}

static void respond(serial_link_t *link, const cJSON *id, int code, const char *text) {
  cJSON *message = envelope(link, "response");
  if (message) {
    if (id) cJSON_AddItemToObject(message, "id", cJSON_Duplicate(id, false));
    cJSON_AddNumberToObject(message, "code", code);
    cJSON_AddBoolToObject(message, "ok", code == 200);
    cJSON_AddStringToObject(message, "message", text);
    if (code == 200) {
      cJSON *status = radio_status_json();
      if (!status) {
        cJSON_Delete(message);
        send_message(link, NULL);
        return;
      }
      cJSON_AddItemToObject(message, "status", status);
    }
  }
  if (code != 200) ESP_LOGW(TAG, "%s: %d %s", link->name, code, text);
  send_message(link, message);
}

static void command(serial_link_t *link) {
  cJSON *json = radio_json_parse(link->line.data, strlen(link->line.data));
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
  const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
  if (!cJSON_IsObject(json) || !cJSON_IsString(cmd) ||
      (id && (!cJSON_IsString(id) || strlen(id->valuestring) > 64))) {
    respond(link, NULL, 400, "Expected object with cmd and optional string id (max 64 bytes)");
  } else if (!strcmp(cmd->valuestring, "status")) {
    respond(link, id, 200, "Current status");
  } else if (!strcmp(cmd->valuestring, "set_stream") ||
             !strcmp(cmd->valuestring, "set_station")) {
    radio_command_t parsed;
    const char *text;
    if (!radio_command_parse(json, &parsed) ||
        (!strcmp(cmd->valuestring, "set_stream") && parsed.station != 4)) {
      respond(link, id, 400, "Invalid radio command or direct stream URL");
    } else {
      int code = radio_apply_command(json, &text);
      respond(link, id, code, text);
    }
  } else {
    respond(link, id, 400, "Unknown cmd; use status, set_stream or set_station");
  }
  cJSON_Delete(json);
}

static void serial_task(void *context) {
  serial_link_t *link = context;
  int64_t last_status = 0;
  for (;;) {
    uart_event_t event;
    if (xQueueReceive(link->events, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (event.type == UART_DATA) {
        uint8_t data[128];
        size_t remaining = event.size;
        while (remaining) {
          size_t count = remaining < sizeof(data) ? remaining : sizeof(data);
          int n = uart_read_bytes(link->port, data, count, pdMS_TO_TICKS(20));
          if (n <= 0) {
            ESP_LOGE(TAG, "%s: UART receive failed", link->name);
            break;
          }
          remaining -= (size_t)n;
          for (int i = 0; i < n; ++i) {
            int result = radio_line_feed(&link->line, data[i]);
            if (result == 1) command(link);
            else if (result == -1)
              respond(link, NULL, 400, "Invalid or oversized line; maximum 1023 bytes");
          }
        }
      } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                 event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR ||
                 event.type == UART_BREAK) {
        ESP_LOGW(TAG, "%s: UART receive event %d; discarding through next newline",
                 link->name, event.type);
        ESP_ERROR_CHECK(uart_flush_input(link->port));
        xQueueReset(link->events);
        memset(&link->line, 0, sizeof(link->line));
        link->line.invalid = true;
        respond(link, NULL, 400, "UART receive error; resynchronize with newline");
      }
    }
    int64_t now = esp_timer_get_time();
    if (now - last_status >= 2000000 || !last_status) {
      cJSON *message = envelope(link, "status");
      cJSON *status = radio_status_json();
      if (message && status) cJSON_AddItemToObject(message, "status", status);
      else {
        cJSON_Delete(message);
        cJSON_Delete(status);
        message = NULL;
      }
      send_message(link, message);
      last_status = now;
    }
  }
}
#endif

esp_err_t radio_serial_init(void) {
#ifdef CONFIG_RADIO_V2
  const uart_config_t config = {
      .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
  for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); ++i) {
    serial_link_t *link = &links[i];
    esp_err_t err = uart_param_config(link->port, &config);
    if (err == ESP_OK)
      err = uart_set_pin(link->port, link->tx, link->rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK)
      err = uart_driver_install(link->port, 4096, 4096, 20, &link->events, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s: initialization failed: %s", link->name, esp_err_to_name(err));
      return err;
    }
    if (xTaskCreatePinnedToCore(serial_task, link->name, 8192, link, 2, NULL, 0) != pdPASS) {
      ESP_LOGE(TAG, "%s: cannot create UART task", link->name);
      return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s: RX%d TX%d 115200 8N1", link->name, link->rx, link->tx);
  }
#endif
  return ESP_OK;
}
