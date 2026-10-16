#include "audio_output.h"
#include "audio_receiver.h"
#include "audio_resample.h"
#include "radio.h"
#include "dit4096.h"
#include "spdif_encoder.h"
#include "led.h"
#include "rtsp_server.h"
#include "driver/i2s_std.h"
#ifdef CONFIG_RADIO_V2
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#endif
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TAG "audio_spdif"
#define OUTPUT_RATE CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define FRAME_SAMPLES 352
#define MAX_OUTPUT_FRAMES 600
#define DMA_FRAMES 384
#define DMA_COUNT 12

_Static_assert(OUTPUT_RATE == 48000, "Radio S/PDIF encoder requires 48 kHz output");

static i2s_chan_handle_t tx;
static TaskHandle_t playback_handle;
static atomic_bool flush_requested;
static atomic_bool stopping;
static atomic_int requested_rate = 44100;
static atomic_uint write_errors;
static uint32_t sent_frames;
static portMUX_TYPE cursor_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t submitted_frames;
static uint32_t lost_frames;
static atomic_uint dma_underruns;
static atomic_uint timing_max_us[4];

static void timing_note(int idx, int64_t start_us) {
  uint32_t dt = (uint32_t)(esp_timer_get_time() - start_us);
  if (dt > atomic_load_explicit(&timing_max_us[idx], memory_order_relaxed))
    atomic_store_explicit(&timing_max_us[idx], dt, memory_order_relaxed);
}

void audio_output_get_timing_us(uint32_t out[4]) {
  for (int i = 0; i < 4; ++i) out[i] = atomic_exchange(&timing_max_us[i], 0);
}
static spdif_encoder_t encoder;

static bool IRAM_ATTR spdif_on_sent(i2s_chan_handle_t handle,
                                    i2s_event_data_t *event, void *context) {
  (void)handle;
  (void)context;
  if (event) {
    portENTER_CRITICAL_ISR(&cursor_lock);
    sent_frames += event->size / 16;
    portEXIT_CRITICAL_ISR(&cursor_lock);
  }
  return false;
}

static void output_cursor_reset(void) {
  portENTER_CRITICAL(&cursor_lock);
  sent_frames = 0;
  portEXIT_CRITICAL(&cursor_lock);
  submitted_frames = 0;
  lost_frames = 0;
}

// Sole writer: playback task, between DMA writes. Differences stay ring-sized,
// so 32-bit modulo counters remain valid across long-running frame wraparound.
static uint32_t output_queued_frames(void) {
  portENTER_CRITICAL(&cursor_lock);
  uint32_t sent = sent_frames;
  portEXIT_CRITICAL(&cursor_lock);
  int32_t queued = (int32_t)(submitted_frames + lost_frames - sent);
  if (queued < 0) {
    uint32_t missing = (uint32_t)(-(int64_t)queued);
    lost_frames += missing;
    atomic_fetch_add(&dma_underruns, 1);
    queued = 0;
  }
  uint32_t ring = DMA_COUNT * (DMA_FRAMES / 2);
  uint32_t frames = (uint32_t)queued > ring ? ring : (uint32_t)queued;
  return frames;
}

static esp_err_t spdif_write(const int16_t *pcm, size_t frames) {
  uint32_t words[96 * 4];
  for (size_t offset = 0; offset < frames;) {
    size_t count = frames - offset;
    if (count > 96) count = 96;
    for (size_t i = 0; i < count; ++i)
      spdif_encode(&encoder, pcm[(offset + i) * 2], pcm[(offset + i) * 2 + 1],
                   words + i * 4);
    size_t written = 0;
    esp_err_t err = i2s_channel_write(tx, words, count * 16, &written, 100);
    submitted_frames += written / 16;
    if (err != ESP_OK || written != count * 16) {
      atomic_fetch_add(&write_errors, 1);
      ESP_LOGE(TAG, "DMA write failed: %s (%u/%u)", esp_err_to_name(err),
               (unsigned)written, (unsigned)(count * 16));
      return err == ESP_OK ? ESP_FAIL : err;
    }
    offset += count;
  }
  return ESP_OK;
}

static void playback_task(void *unused) {
  (void)unused;
  int16_t *pcm = malloc((FRAME_SAMPLES + 1) * 4);
  int16_t *output = malloc(MAX_OUTPUT_FRAMES * 4);
  if (!pcm || !output) {
    ESP_LOGE(TAG, "Cannot allocate playback buffers");
    free(pcm);
    free(output);
    playback_handle = NULL;
    vTaskDelete(NULL);
    return;
  }
  bool previous_airplay = false;
  uint32_t rate = 0;
  uint32_t generation = 0;
  int64_t last_audio_us = 0;
  int32_t gain = 32768;
  while (!atomic_load(&stopping)) {
    bool ap = radio_airplay_active();
    uint32_t next_generation = radio_generation();
    if (ap != previous_airplay || generation != next_generation ||
        atomic_exchange(&flush_requested, false)) {
      ESP_ERROR_CHECK(i2s_channel_disable(tx));
      dit4096_flush();
      output_cursor_reset();
      memset(&encoder, 0, sizeof(encoder));
      ESP_ERROR_CHECK(i2s_channel_enable(tx));
      audio_resample_reset();
      last_audio_us = 0;
      rate = 0;
      previous_airplay = ap;
      generation = next_generation;
    }
    uint32_t source_rate = atomic_load(&requested_rate);
    int64_t loop_start_us = esp_timer_get_time();
    size_t frames = ap ? audio_receiver_read(pcm, FRAME_SAMPLES + 1) :
                        radio_read(pcm, FRAME_SAMPLES, &source_rate);
    timing_note(0, loop_start_us);
    int16_t *data = pcm;
    if (frames) {
      dit4096_submit(pcm, frames, source_rate, ap ? airplay_get_volume_q15() : 32768);
      if (source_rate != rate) {
        if (!audio_resample_init(source_rate, OUTPUT_RATE, 2)) {
          ESP_LOGE(TAG, "Cannot configure %lu Hz resampling", (unsigned long)source_rate);
          frames = 0;
        } else rate = source_rate;
      }
      if (frames && audio_resample_is_active()) {
        int64_t resample_start_us = esp_timer_get_time();
        frames = audio_resample_process(pcm, frames, output, MAX_OUTPUT_FRAMES);
        timing_note(1, resample_start_us);
        data = output;
      }
      if (ap) {
        int32_t target = airplay_get_volume_q15();
        for (size_t i = 0; i < frames; ++i) {
          int32_t delta = target - gain;
          if (delta) gain += delta / 256 ? delta / 256 : (delta > 0 ? 1 : -1);
          data[i * 2] = (int32_t)data[i * 2] * gain >> 15;
          data[i * 2 + 1] = (int32_t)data[i * 2 + 1] * gain >> 15;
        }
      } else gain = 32768;
    }
    int64_t now_us = esp_timer_get_time();
    if (frames && (!ap || audio_receiver_is_playing())) last_audio_us = now_us;
    led_set_playback(last_audio_us != 0 && now_us - last_audio_us < 250000, ap);
    if (!frames) {
      memset(pcm, 0, FRAME_SAMPLES * 4);
      data = pcm;
      frames = FRAME_SAMPLES;
    }
    output_queued_frames();
    int64_t write_start_us = esp_timer_get_time();
    if (spdif_write(data, frames) != ESP_OK)
      vTaskDelay(pdMS_TO_TICKS(10));
    timing_note(2, write_start_us);
    timing_note(3, loop_start_us);
  }
  free(pcm);
  free(output);
  led_set_playback(false, false);
  dit4096_flush();
  playback_handle = NULL;
  vTaskDelete(NULL);
}

esp_err_t audio_output_init(void) {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel.dma_desc_num = DMA_COUNT;
  channel.dma_frame_num = DMA_FRAMES;
  channel.auto_clear = true;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &tx, NULL), TAG, "I2S allocation");
  i2s_std_config_t cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(OUTPUT_RATE * 2),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = I2S_GPIO_UNUSED,
                   .ws = I2S_GPIO_UNUSED, .dout = CONFIG_SPDIF_DO_IO,
                   .din = I2S_GPIO_UNUSED}};
  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &cfg), TAG, "I2S configuration");
#ifdef CONFIG_RADIO_V2
  ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_NUM_47, GPIO_MODE_OUTPUT), TAG, "Coax GPIO");
  esp_rom_gpio_connect_out_signal(GPIO_NUM_47, I2S0O_SD_OUT_IDX, false, false);
  ESP_LOGI(TAG, "S/PDIF mirrored to GPIO47; Sony PCM output disabled");
#endif
  i2s_event_callbacks_t callbacks = {.on_sent = spdif_on_sent};
  ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(tx, &callbacks, NULL),
                      TAG, "I2S completion callback");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "I2S enable");
  ESP_RETURN_ON_ERROR(dit4096_init(), TAG, "DIT4096 initialization");
  ESP_LOGI(TAG, "48 kHz S/PDIF output on GPIO%d", CONFIG_SPDIF_DO_IO);
  return ESP_OK;
}

void audio_output_start(void) {
  if (playback_handle) return;
  atomic_store(&stopping, false);
  if (xTaskCreatePinnedToCore(playback_task, "spdif_play", 6144, NULL,
                             AUDIO_PLAYBACK_TASK_PRIORITY, &playback_handle, 1) != pdPASS)
    ESP_LOGE(TAG, "Cannot start S/PDIF playback task");
}

void audio_output_stop(void) {
  atomic_store(&stopping, true);
  while (playback_handle) vTaskDelay(pdMS_TO_TICKS(10));
  dit4096_flush();
}

void audio_output_flush(void) {
  dit4096_flush();
  atomic_store(&flush_requested, true);
}
void audio_output_set_source_rate(int rate) {
  if (rate > 0) atomic_store(&requested_rate, rate);
}
void audio_output_set_sample_rate(uint32_t rate) {
  // The physical output is fixed at 48 kHz; source audio is resampled.
  audio_output_set_source_rate(rate);
}
esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  (void)wait;
  if (playback_handle) return ESP_ERR_INVALID_STATE;
  if (bytes % 4) return ESP_ERR_INVALID_SIZE;
  const int16_t *pcm = data;
  for (size_t offset = 0; offset < bytes / 4;) {
    size_t count = bytes / 4 - offset;
    if (count > FRAME_SAMPLES) count = FRAME_SAMPLES;
    // Direct writes already use the physical 48 kHz S/PDIF format.
    dit4096_submit(pcm + offset * 2, count, OUTPUT_RATE, 32768);
    esp_err_t err = spdif_write(pcm + offset * 2, count);
    if (err != ESP_OK) return err;
    offset += count;
  }
  return ESP_OK;
}
uint32_t audio_output_get_hardware_latency_us(void) {
  return DMA_COUNT * (DMA_FRAMES / 2) * 1000000UL / OUTPUT_RATE;
}
bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us) {
  if (!now_us || !pipeline_us) return false;
  uint32_t queued = output_queued_frames();
  *now_us = esp_timer_get_time();
  *pipeline_us = queued * 1000000UL / OUTPUT_RATE;
  return true;
}
uint32_t audio_output_get_underruns(void) { return atomic_load(&dma_underruns); }
uint32_t audio_output_get_write_errors(void) { return atomic_load(&write_errors); }
