#include "radio.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "icy_metadata.h"
#include "radio_command.h"
#include "rtsp_server.h"
#include "settings.h"
#include "dit4096.h"

#include "cJSON.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "nvs.h"
#include "rtsp_events.h"
#include "wifi.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define URL_SIZE 512
#define COMPRESSED_BYTES (192 * 1024)
#define PREBUFFER_BYTES (48 * 1024)
#define PCM_BYTES (128 * 1024)
#define PCM_MAX_BYTES 16384

static const char *TAG = "radio";
static const char *names[] = {"NPO Radio 2", "BNR Radio", "Radio 538", "SLAM! FM"};
static const char *urls[] = {
    "http://icecast.omroep.nl/radio2-bb-mp3",
    "http://playerservices.streamtheworld.com/api/livestream-redirect/BNR_NIEUWSRADIO.mp3",
    "http://playerservices.streamtheworld.com/api/livestream-redirect/RADIO538.mp3",
    "http://playerservices.streamtheworld.com/api/livestream-redirect/SLAM_MP3.mp3"};

static SemaphoreHandle_t lock;
static SemaphoreHandle_t command_lock;
static StreamBufferHandle_t compressed;
static StaticStreamBuffer_t compressed_control;
static RingbufHandle_t pcm_queue;
static TaskHandle_t fetch_task_handle;
static atomic_bool airplay;
static atomic_bool fetching;
static atomic_bool ready;
static atomic_bool decoding;
static atomic_uint generation = 1;
static atomic_uint stream_generation;
static atomic_uint sessions;
static atomic_uint decoder_errors;
static atomic_uint underflows;
static atomic_uint frames_played;
static atomic_int codec;
static uint32_t metadata_interval;
static char radio_title[512];
static char stream_name[160];
static char stream_content_type[96];
static char stream_bitrate[32];
static uint32_t radio_rate;
static int radio_channels;
static rtsp_metadata_t airplay_metadata;
static int selected;
static char custom_url[URL_SIZE];
static char stream_url[URL_SIZE];
static char error_text[160];

typedef struct {
  uint32_t generation;
  uint32_t rate;
  uint32_t bytes;
  int16_t samples[];
} pcm_item_t;

static bool current(unsigned gen) {
  return gen == atomic_load(&generation) && !atomic_load(&airplay);
}

static void error(const char *message) {
  ESP_LOGE(TAG, "%s", message);
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(error_text, message, sizeof(error_text));
  xSemaphoreGive(lock);
}

static void airplay_event(rtsp_event_t event, const rtsp_event_data_t *data, void *ctx) {
  (void)ctx;
  if (event == RTSP_EVENT_METADATA && data) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (data->metadata.has_track_info) airplay_metadata = data->metadata;
    xSemaphoreGive(lock);
  }
  if (event == RTSP_EVENT_CLIENT_CONNECTED || event == RTSP_EVENT_PLAYING) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!atomic_exchange(&airplay, true)) {
      memset(&airplay_metadata, 0, sizeof(airplay_metadata));
      atomic_fetch_add(&generation, 1);
      ESP_LOGI(TAG, "AirPlay connected; radio yields the output");
    }
    xSemaphoreGive(lock);
  } else if (event == RTSP_EVENT_DISCONNECTED) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (atomic_exchange(&airplay, false)) {
      memset(&airplay_metadata, 0, sizeof(airplay_metadata));
      atomic_fetch_add(&generation, 1);
      ESP_LOGI(TAG, "AirPlay disconnected; resuming selected radio");
    }
    xSemaphoreGive(lock);
  }
}

static esp_err_t http_event(esp_http_client_event_t *event) {
  if (event->event_id == HTTP_EVENT_ON_HEADER) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!strcasecmp(event->header_key, "Content-Type")) {
      strlcpy(stream_content_type, event->header_value, sizeof(stream_content_type));
      if (!strncasecmp(event->header_value, "audio/mpeg", 10) ||
          !strncasecmp(event->header_value, "audio/mp3", 9))
        atomic_store(&codec, ESP_AUDIO_SIMPLE_DEC_TYPE_MP3);
      else if (!strncasecmp(event->header_value, "audio/aac", 9))
        atomic_store(&codec, ESP_AUDIO_SIMPLE_DEC_TYPE_AAC);
      else
        atomic_store(&codec, ESP_AUDIO_SIMPLE_DEC_TYPE_NONE);
    } else if (!strcasecmp(event->header_key, "icy-metaint")) {
      char *end;
      unsigned long interval = strtoul(event->header_value, &end, 10);
      if (*end || !interval || interval > UINT32_MAX) {
        ESP_LOGW(TAG, "Invalid ICY metadata interval");
        metadata_interval = UINT32_MAX;
      } else metadata_interval = (uint32_t)interval;
    } else if (!strcasecmp(event->header_key, "icy-name")) {
      strlcpy(stream_name, event->header_value, sizeof(stream_name));
    } else if (!strcasecmp(event->header_key, "icy-br")) {
      strlcpy(stream_bitrate, event->header_value, sizeof(stream_bitrate));
    }
    xSemaphoreGive(lock);
  }
  return ESP_OK;
}

static void fetch_task(void *unused) {
  (void)unused;
  uint8_t chunk[2048];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    unsigned gen = atomic_load(&stream_generation);
    char url[URL_SIZE];
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(url, stream_url, sizeof(url));
    xSemaphoreGive(lock);
    atomic_store(&codec, ESP_AUDIO_SIMPLE_DEC_TYPE_NONE);
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 2500, .event_handler = http_event,
        .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 2048,
        .max_redirection_count = 5};
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
      error("Cannot allocate stream HTTP client");
    } else {
      esp_http_client_set_header(http, "Icy-MetaData", "1");
      esp_err_t result = ESP_FAIL;
      int status = 0;
      for (unsigned redirects = 0; redirects <= 5 && current(gen); ++redirects) {
        atomic_store(&codec, ESP_AUDIO_SIMPLE_DEC_TYPE_NONE);
        xSemaphoreTake(lock, portMAX_DELAY);
        metadata_interval = 0;
        stream_name[0] = stream_content_type[0] = stream_bitrate[0] = 0;
        xSemaphoreGive(lock);
        result = esp_http_client_open(http, 0);
        if (result != ESP_OK) break;
        if (esp_http_client_fetch_headers(http) < 0) {
          result = ESP_FAIL;
          break;
        }
        status = esp_http_client_get_status_code(http);
        if (status < 300 || status >= 400) break;
        result = esp_http_client_set_redirection(http);
        esp_http_client_close(http);
        if (result != ESP_OK) break;
      }
      if (current(gen)) {
        if (result != ESP_OK || status != 200) {
          char message[120];
          snprintf(message, sizeof(message), "Stream HTTP failed (%d, %s)",
                   status, esp_err_to_name(result));
          error(message);
        } else if (atomic_load(&codec) <= 0 || metadata_interval == UINT32_MAX) {
          error("Use a direct MP3/AAC stream with a valid ICY metadata interval");
        } else {
          icy_metadata_t *icy = malloc(sizeof(*icy));
          if (!icy) {
            error("Cannot allocate stream metadata parser");
            esp_http_client_close(http);
            esp_http_client_cleanup(http);
            atomic_store(&ready, false);
            atomic_store(&fetching, false);
            continue;
          }
          icy_metadata_init(icy, metadata_interval);
          atomic_store(&ready, true);
          ESP_LOGI(TAG, "Stream connected; prebuffering");
          int64_t last_data = esp_timer_get_time();
          while (current(gen) && wifi_is_connected()) {
            int n = esp_http_client_read(http, (char *)chunk, sizeof(chunk));
            if (n > 0) {
              last_data = esp_timer_get_time();
              bool updated;
              char title[sizeof(radio_title)];
              n = (int)icy_metadata_filter(icy, chunk, (size_t)n, &updated, title, sizeof(title));
              if (updated && current(gen)) {
                xSemaphoreTake(lock, portMAX_DELAY);
                if (current(gen)) strlcpy(radio_title, title, sizeof(radio_title));
                xSemaphoreGive(lock);
              }
              size_t sent = 0;
              while (sent < (size_t)n && current(gen))
                sent += xStreamBufferSend(compressed, chunk + sent, n - sent,
                                          pdMS_TO_TICKS(50));
            } else if ((n < 0 && n != -ESP_ERR_HTTP_EAGAIN) ||
                       esp_http_client_is_complete_data_received(http) ||
                       esp_timer_get_time() - last_data > 10000000) {
              ESP_LOGW(TAG, "Stream ended: read=%d errno=%d", n,
                       esp_http_client_get_errno(http));
              error("Radio stream disconnected or timed out; reconnecting");
              break;
            } else {
              vTaskDelay(pdMS_TO_TICKS(10));
            }
          }
          free(icy);
        }
      }
      esp_http_client_close(http);
      esp_http_client_cleanup(http);
    }
    atomic_store(&ready, false);
    atomic_store(&fetching, false);
  }
}

static void decode_session(unsigned gen) {
  esp_audio_simple_dec_handle_t dec = NULL;
  esp_audio_simple_dec_cfg_t cfg = {.dec_type = atomic_load(&codec)};
  esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
  if (cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) {
    aac.aac_plus_enable = true;
    cfg.dec_cfg = &aac;
    cfg.cfg_size = sizeof(aac);
  }
  if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
    error("Cannot open radio MP3/AAC decoder");
    return;
  }
  uint8_t *input = malloc(8192);
  pcm_item_t *item = malloc(sizeof(*item) + PCM_MAX_BYTES);
  if (!input || !item) {
    error("Cannot allocate radio decoding buffers");
  } else {
    size_t buffered = 0;
    unsigned stalled = 0;
    while (current(gen)) {
      if (buffered < 4096 || stalled) {
        size_t n = xStreamBufferReceive(compressed, input + buffered,
                                       8192 - buffered, pdMS_TO_TICKS(100));
        buffered += n;
        if (!n && !buffered) {
          atomic_fetch_add(&underflows, 1);
          error("Radio buffer empty; reconnecting");
          break;
        }
      }
      esp_audio_simple_dec_raw_t raw = {.buffer = input, .len = buffered};
      esp_audio_simple_dec_out_t out = {
          .buffer = (uint8_t *)item->samples, .len = PCM_MAX_BYTES};
      esp_audio_err_t result = esp_audio_simple_dec_process(dec, &raw, &out);
      if (raw.consumed > buffered) {
        error("Decoder reported invalid consumed byte count");
        break;
      }
      buffered -= raw.consumed;
      memmove(input, input + raw.consumed, buffered);
      if (result != ESP_AUDIO_ERR_OK && result != ESP_AUDIO_ERR_DATA_LACK) {
        atomic_fetch_add(&decoder_errors, 1);
        ESP_LOGW(TAG, "Radio decoder status: %d", result);
        if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
          error("Radio codec output exceeds supported frame size");
          break;
        }
      }
      if (out.decoded_size) {
        esp_audio_simple_dec_info_t info = {0};
        if (esp_audio_simple_dec_get_info(dec, &info) != ESP_AUDIO_ERR_OK ||
            info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2) ||
            (info.sample_rate != 32000 && info.sample_rate != 44100 &&
             info.sample_rate != 48000)) {
          error("Radio must be 16-bit MP3/AAC at 32, 44.1 or 48 kHz");
          break;
        }
        if (info.channel == 1) {
          if (out.decoded_size * 2 > PCM_MAX_BYTES) {
            error("Mono radio frame exceeds buffer capacity");
            break;
          }
          for (size_t i = out.decoded_size / 2; i-- > 0;) {
            item->samples[i * 2] = item->samples[i];
            item->samples[i * 2 + 1] = item->samples[i];
          }
          out.decoded_size *= 2;
        }
        item->generation = gen;
        item->rate = info.sample_rate;
        item->bytes = out.decoded_size;
        xSemaphoreTake(lock, portMAX_DELAY);
        radio_rate = info.sample_rate;
        radio_channels = info.channel;
        xSemaphoreGive(lock);
        atomic_store(&decoding, true);
        while (current(gen) &&
               xRingbufferSend(pcm_queue, item, sizeof(*item) + item->bytes,
                               pdMS_TO_TICKS(50)) != pdTRUE) {}
      }
      if (!raw.consumed && !out.decoded_size) {
        if (++stalled > 10 || buffered == 8192) {
          error("Radio decoder made no progress");
          break;
        }
      } else stalled = 0;
    }
  }
  free(input);
  free(item);
  esp_audio_simple_dec_close(dec);
}

static void decode_task(void *unused) {
  (void)unused;
  for (;;) {
    if (atomic_load(&airplay) || !wifi_is_connected()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    unsigned gen = atomic_load(&generation);
    if (xStreamBufferReset(compressed) != pdPASS) {
      error("Cannot reset compressed buffer");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    atomic_store(&stream_generation, gen);
    atomic_store(&fetching, true);
    atomic_store(&ready, false);
    atomic_fetch_add(&sessions, 1);
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(stream_url, selected == 4 ? custom_url : urls[selected], sizeof(stream_url));
    error_text[0] = 0;
    radio_title[0] = 0;
    radio_rate = 0;
    radio_channels = 0;
    xSemaphoreGive(lock);
    xTaskNotifyGive(fetch_task_handle);
    int64_t start = esp_timer_get_time();
    while (current(gen) && atomic_load(&fetching) &&
           xStreamBufferBytesAvailable(compressed) < PREBUFFER_BYTES &&
           esp_timer_get_time() - start < 15000000)
      vTaskDelay(pdMS_TO_TICKS(20));
    if (current(gen) && atomic_load(&ready) &&
        xStreamBufferBytesAvailable(compressed) >= PREBUFFER_BYTES)
      decode_session(gen);
    else if (current(gen) && atomic_load(&fetching))
      error("Radio prebuffer timeout");
    atomic_store(&decoding, false);
    // End this fetch session without overriding a newer station selection.
    unsigned expected = gen;
    atomic_compare_exchange_strong(&generation, &expected, gen + 1);
    while (atomic_load(&fetching)) vTaskDelay(pdMS_TO_TICKS(10));
    if (expected == gen && !atomic_load(&airplay)) vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

bool radio_airplay_active(void) { return atomic_load(&airplay); }
uint32_t radio_generation(void) { return atomic_load(&generation); }

size_t radio_read(int16_t *pcm, size_t frames, uint32_t *rate) {
  static pcm_item_t *item;
  static size_t offset;
  for (unsigned discarded = 0; discarded < 32; ++discarded) {
    if (item && (!current(item->generation) || offset == item->bytes)) {
      vRingbufferReturnItem(pcm_queue, item);
      item = NULL;
    }
    if (!item) {
      size_t length;
      item = xRingbufferReceive(pcm_queue, &length, 0);
      offset = 0;
      if (!item) return 0;
      if (length < sizeof(*item) || length != sizeof(*item) + item->bytes ||
          item->bytes % 4) {
        error("Invalid queued PCM frame");
        vRingbufferReturnItem(pcm_queue, item);
        item = NULL;
        return 0;
      }
    }
    if (!current(item->generation)) continue;
    size_t n = (item->bytes - offset) / 4;
    if (n > frames) n = frames;
    memcpy(pcm, (uint8_t *)item->samples + offset, n * 4);
    offset += n * 4;
    *rate = item->rate;
    atomic_fetch_add(&frames_played, n);
    return n;
  }
  return 0;
}

cJSON *radio_status_json(void) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;
  audio_diagnostics_t diagnostics;
  audio_receiver_get_diagnostics(&diagnostics);
  xSemaphoreTake(lock, portMAX_DELAY);
  cJSON_AddNumberToObject(root, "selected", selected);
  cJSON_AddStringToObject(root, "station", selected == 4 ? "Custom stream" : names[selected]);
  cJSON_AddStringToObject(root, "custom_url", custom_url);
  cJSON_AddStringToObject(root, "error", error_text);
  bool ap = atomic_load(&airplay);
  cJSON *now_playing = cJSON_AddObjectToObject(root, "now_playing");
  if (!now_playing) {
    xSemaphoreGive(lock);
    cJSON_Delete(root);
    return NULL;
  }
  cJSON_AddStringToObject(now_playing, "title", ap ? airplay_metadata.title : radio_title);
  cJSON_AddStringToObject(now_playing, "artist", ap ? airplay_metadata.artist : "");
  cJSON_AddStringToObject(now_playing, "album", ap ? airplay_metadata.album : "");
  cJSON_AddStringToObject(now_playing, "stream_name", ap ? "" : stream_name);
  cJSON_AddStringToObject(now_playing, "content_type", ap ? "" : stream_content_type);
  cJSON_AddStringToObject(now_playing, "bitrate_kbps", ap ? "" : stream_bitrate);
  cJSON_AddStringToObject(now_playing, "url", ap ? "" : stream_url);
  cJSON_AddNumberToObject(now_playing, "sample_rate", ap ? diagnostics.format.sample_rate : radio_rate);
  cJSON_AddNumberToObject(now_playing, "channels", ap ? diagnostics.format.channels : radio_channels);
  cJSON_AddBoolToObject(now_playing, "metadata_supported", ap || metadata_interval > 0);
  xSemaphoreGive(lock);
  cJSON_AddStringToObject(root, "source", ap ? "AirPlay" : "Radio");
  cJSON_AddBoolToObject(root, "radio_playing", !ap && atomic_load(&decoding));
  cJSON_AddNumberToObject(root, "sessions", atomic_load(&sessions));
  cJSON_AddNumberToObject(root, "decoder_errors", atomic_load(&decoder_errors));
  cJSON_AddNumberToObject(root, "buffer_underflows", atomic_load(&underflows));
  cJSON_AddNumberToObject(root, "frames_played", atomic_load(&frames_played));
  cJSON_AddNumberToObject(root, "buffer_bytes", xStreamBufferBytesAvailable(compressed));
  cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
  cJSON_AddNumberToObject(root, "spdif_gpio", CONFIG_SPDIF_DO_IO);
#ifdef CONFIG_RADIO_V2
  cJSON_AddNumberToObject(root, "protocol_version", 2);
  cJSON_AddBoolToObject(root, "radio_changes_blocked", ap);
  cJSON_AddNumberToObject(root, "coax_gpio", 47);
  cJSON_AddBoolToObject(root, "sony_pcm_enabled", false);
  dit4096_status_t dit;
  dit4096_get_status(&dit);
  cJSON_AddBoolToObject(root, "dit_pcm_enabled", dit.enabled);
  cJSON *dit_status = cJSON_AddObjectToObject(root, "dit");
  if (!dit_status) {
    cJSON_Delete(root);
    return NULL;
  }
  cJSON_AddBoolToObject(dit_status, "running", dit.running);
  cJSON_AddNumberToObject(dit_status, "sample_rate", dit.sample_rate);
  cJSON_AddNumberToObject(dit_status, "mclk_hz", dit.sample_rate * 256);
  cJSON_AddNumberToObject(dit_status, "bclk_hz", dit.sample_rate * 64);
  cJSON_AddNumberToObject(dit_status, "pcm_bits", 16);
  cJSON_AddNumberToObject(dit_status, "container_bits", 24);
  cJSON_AddNumberToObject(dit_status, "slot_bits", 32);
  cJSON_AddNumberToObject(dit_status, "write_errors", dit.write_errors);
  cJSON_AddNumberToObject(dit_status, "queue_overflows", dit.queue_overflows);
  cJSON_AddNumberToObject(dit_status, "starvation_resets", dit.starvation_resets);
  cJSON_AddNumberToObject(dit_status, "frames_submitted", dit.frames_submitted);
  cJSON_AddNumberToObject(dit_status, "queue_min_free_bytes", dit.queue_min_free_bytes);
  cJSON_AddNumberToObject(dit_status, "max_write_ms", dit.max_write_ms);
  cJSON_AddNumberToObject(dit_status, "overflow_stall_ms", dit.overflow_stall_ms);
  cJSON_AddBoolToObject(dit_status, "overflow_running", dit.overflow_running);
#endif
  cJSON_AddNumberToObject(root, "spdif_errors", audio_output_get_write_errors());
  cJSON_AddNumberToObject(root, "spdif_dma_underruns", audio_output_get_underruns());
  uint32_t timing_us[4];
  audio_output_get_timing_us(timing_us);
  cJSON_AddNumberToObject(root, "loop_max_read_us", timing_us[0]);
  cJSON_AddNumberToObject(root, "loop_max_resample_us", timing_us[1]);
  cJSON_AddNumberToObject(root, "loop_max_write_us", timing_us[2]);
  cJSON_AddNumberToObject(root, "loop_max_total_us", timing_us[3]);
  cJSON_AddNumberToObject(root, "internal_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  cJSON_AddStringToObject(now_playing, "codec", ap ? diagnostics.format.codec :
      (atomic_load(&codec) == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 ? "MP3" :
       atomic_load(&codec) == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC ? "AAC" : ""));
  cJSON_AddNumberToObject(now_playing, "output_sample_rate", CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  cJSON_AddNumberToObject(now_playing, "output_bits_per_sample", 16);
  cJSON_AddNumberToObject(now_playing, "source_bits_per_sample",
                          ap ? diagnostics.format.bits_per_sample : 0);
  cJSON *airplay_status = cJSON_AddObjectToObject(root, "airplay");
  if (!airplay_status) {
    cJSON_Delete(root);
    return NULL;
  }
  cJSON_AddNumberToObject(airplay_status, "stream_type", diagnostics.stream_type);
  cJSON_AddBoolToObject(airplay_status, "playing", ap && audio_receiver_is_playing());
  cJSON_AddNumberToObject(airplay_status, "buffered_frames", diagnostics.buffered_frames);
  cJSON_AddNumberToObject(airplay_status, "gaps", diagnostics.gaps);
  cJSON_AddNumberToObject(airplay_status, "packets_received", diagnostics.stats.packets_received);
  cJSON_AddNumberToObject(airplay_status, "chunks_decoded", diagnostics.stats.packets_decoded);
  cJSON_AddNumberToObject(airplay_status, "packets_dropped", diagnostics.stats.packets_dropped);
  cJSON_AddNumberToObject(airplay_status, "decrypt_errors", diagnostics.stats.decrypt_errors);
  cJSON_AddNumberToObject(airplay_status, "decoder_errors", diagnostics.stats.decoder_errors);
  cJSON_AddNumberToObject(airplay_status, "packet_queue_overflows", diagnostics.stats.packet_queue_overflows);
  cJSON_AddNumberToObject(airplay_status, "buffer_underflows", diagnostics.stats.buffer_underruns);
  cJSON_AddNumberToObject(airplay_status, "buffer_overflows", diagnostics.stats.buffer_overruns);
  cJSON_AddNumberToObject(airplay_status, "late_frames", diagnostics.stats.late_frames);
  float volume_db = 0;
  if (settings_get_volume(&volume_db) == ESP_OK)
    cJSON_AddNumberToObject(airplay_status, "volume_db", volume_db);
  cJSON_AddNumberToObject(airplay_status, "volume_q15", airplay_get_volume_q15());
  cJSON_AddNumberToObject(airplay_status, "volume_updates", rtsp_conn_volume_updates());
  return root;
}

static esp_err_t status_handler(httpd_req_t *req) {
  cJSON *root = radio_status_json();
  char *json = root ? cJSON_PrintUnformatted(root) : NULL;
  cJSON_Delete(root);
  if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  free(json);
  return result;
}

int radio_apply_command(const cJSON *json, const char **message) {
  radio_command_t command;
  *message = "Choose station 0-4 or a direct HTTP/HTTPS URL; override must be boolean";
  if (!radio_command_parse(json, &command)) return 400;
#ifndef CONFIG_RADIO_V2
  if (command.override) {
    *message = "AirPlay override requires V2 firmware";
    return 400;
  }
#endif
  if (xSemaphoreTake(command_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
    *message = "Another radio command is in progress; retry after its response";
    ESP_LOGW(TAG, "%s", *message);
    return 503;
  }
  bool blocked = false;
#ifdef CONFIG_RADIO_V2
  if (command.override) {
    blocked = true;
    if (rtsp_server_override_begin(5000) != ESP_OK) {
      rtsp_server_override_end();
      xSemaphoreGive(command_lock);
      *message = "AirPlay disconnect timed out; radio selection not changed";
      ESP_LOGE(TAG, "%s", *message);
      return 503;
    }
  }
#endif
  xSemaphoreTake(lock, portMAX_DELAY);
#ifdef CONFIG_RADIO_V2
  if (atomic_load(&airplay)) {
    xSemaphoreGive(lock);
    if (blocked) rtsp_server_override_end();
    xSemaphoreGive(command_lock);
    *message = "AirPlay owns playback; use override:true to disconnect it";
    ESP_LOGW(TAG, "%s", *message);
    return 409;
  }
#endif
  if (command.station == 4) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("radio", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
      err = nvs_set_str(nvs, "custom_url", command.url);
      if (err == ESP_OK) err = nvs_commit(nvs);
      nvs_close(nvs);
    }
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Cannot save custom URL: %s", esp_err_to_name(err));
      xSemaphoreGive(lock);
      if (blocked) rtsp_server_override_end();
      xSemaphoreGive(command_lock);
      *message = "Cannot save custom stream";
      return 500;
    }
  }
  selected = command.station;
  if (selected == 4) strlcpy(custom_url, command.url, sizeof(custom_url));
  error_text[0] = 0;
  radio_title[0] = stream_name[0] = stream_content_type[0] = stream_bitrate[0] = 0;
  stream_url[0] = 0;
  metadata_interval = 0;
  radio_rate = 0;
  radio_channels = 0;
  atomic_store(&decoding, false);
  atomic_fetch_add(&generation, 1);
  xSemaphoreGive(lock);
  if (blocked) rtsp_server_override_end();
  xSemaphoreGive(command_lock);
  *message = "Selection accepted; playback may still be buffering";
  return 200;
}

static esp_err_t select_handler(httpd_req_t *req) {
  char body[1024];
  if (!req->content_len || req->content_len >= sizeof(body))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
  size_t received = 0;
  while (received < req->content_len) {
    int n = httpd_req_recv(req, body + received, req->content_len - received);
    if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete request");
    received += n;
  }
  body[received] = 0;
  cJSON *json = radio_json_parse(body, received);
  const char *message;
  int code = radio_apply_command(json, &message);
  cJSON_Delete(json);
  if (code != 200) {
    httpd_resp_set_status(req, code == 409 ? "409 Conflict" :
        code == 503 ? "503 Service Unavailable" : code == 500 ?
        "500 Internal Server Error" : "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
  }
  return status_handler(req);
}

esp_err_t radio_register_http(httpd_handle_t server) {
  httpd_uri_t status = {.uri = "/api/radio", .method = HTTP_GET, .handler = status_handler};
  httpd_uri_t select = {.uri = "/api/radio", .method = HTTP_POST, .handler = select_handler};
  esp_err_t result = httpd_register_uri_handler(server, &status);
  if (result == ESP_OK) result = httpd_register_uri_handler(server, &select);
  return result;
}

esp_err_t radio_init(void) {
  esp_err_t control_err = rtsp_server_control_init();
  if (control_err != ESP_OK) return control_err;
  lock = xSemaphoreCreateMutex();
  command_lock = xSemaphoreCreateMutex();
  if (!lock || !command_lock) return ESP_ERR_NO_MEM;
  uint8_t *storage = heap_caps_malloc(COMPRESSED_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!storage) return ESP_ERR_NO_MEM;
  compressed = xStreamBufferCreateStatic(COMPRESSED_BYTES + 1, 1, storage, &compressed_control);
  pcm_queue = xRingbufferCreateWithCaps(PCM_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
  if (!compressed || !pcm_queue) return ESP_ERR_NO_MEM;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open("radio", NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    size_t size = sizeof(custom_url);
    err = nvs_get_str(nvs, "custom_url", custom_url, &size);
    nvs_close(nvs);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
      ESP_LOGE(TAG, "Cannot load custom URL: %s", esp_err_to_name(err));
  } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
  if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
      esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return ESP_FAIL;
  if (rtsp_events_register(airplay_event, NULL) != 0) return ESP_FAIL;
  if (xTaskCreatePinnedToCore(fetch_task, "radio_net", 6144, NULL, 3,
                             &fetch_task_handle, 0) != pdPASS ||
      xTaskCreatePinnedToCore(decode_task, "radio_dec", 8192, NULL, 4,
                             NULL, 1) != pdPASS) return ESP_ERR_NO_MEM;
  return ESP_OK;
}
