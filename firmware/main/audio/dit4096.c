#include "dit4096.h"
#include "sdkconfig.h"
#include <string.h>

// The DIT4096 output is opt-in: the board is not populated by default.
#if defined(CONFIG_RADIO_V2) && defined(CONFIG_RADIO_DIT4096_PCM)
#define DIT4096_ACTIVE 1
#endif

#ifdef DIT4096_ACTIVE
#include "dit4096_format.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include <stdatomic.h>

#define TAG "dit4096"
#define DIT_FRAMES 353
#define DIT_DMA_FRAMES 192
#define DIT_DMA_COUNT 16
#define DIT_PREBUFFER_FRAMES (DIT_DMA_FRAMES * DIT_DMA_COUNT)
#define DIT_RESET_GPIO GPIO_NUM_17

typedef struct {
  uint32_t generation;
  uint32_t rate;
  uint32_t frames;
  int16_t samples[DIT_FRAMES * 2];
} dit_packet_t;

static RingbufHandle_t queue;
static i2s_chan_handle_t tx;
static atomic_uint generation = 1;
static atomic_bool enabled;
static atomic_bool running;
static atomic_uint sample_rate;
static atomic_uint write_errors;
static atomic_uint queue_overflows;
static atomic_uint starvation_resets;
static atomic_uint frames_submitted;
static atomic_uint queue_min_free = UINT32_MAX;
static atomic_uint max_write_ms;
static atomic_llong last_consume_us;
static atomic_uint overflow_stall_ms;
static atomic_bool overflow_running;
// Single producer: the shared playback task (or direct writer when stopped).
static dit_packet_t outgoing;
static uint32_t words[DIT_FRAMES * 2];
static int32_t gain = 32768;

static void disable_output(void) {
  ESP_ERROR_CHECK(gpio_set_level(DIT_RESET_GPIO, 0));
  // Reset the DMA preload cursor even if a source ended during prebuffering.
  if (!atomic_exchange(&running, false)) ESP_ERROR_CHECK(i2s_channel_enable(tx));
  ESP_ERROR_CHECK(i2s_channel_disable(tx));
  atomic_store(&sample_rate, 0);
}

static void dit_task(void *unused) {
  (void)unused;
  uint32_t active_generation = 0, rate = 0, preloaded = 0;
  int64_t last_packet_us = 0;
  for (;;) {
    size_t size;
    dit_packet_t *packet = xRingbufferReceive(queue, &size, pdMS_TO_TICKS(20));
    uint32_t current = atomic_load(&generation);
    atomic_store(&last_consume_us, esp_timer_get_time());
    if (current != active_generation) {
      disable_output();
      rate = preloaded = 0;
      active_generation = current;
    }
    if (!packet) {
      if (rate && esp_timer_get_time() - last_packet_us > 100000) {
        atomic_fetch_add(&starvation_resets, 1);
        ESP_LOGW(TAG, "PCM queue empty; muting and rebuffering native output");
        disable_output();
        rate = preloaded = 0;
      }
      continue;
    }
    if (size != sizeof(*packet) || packet->generation != current) {
      if (size != sizeof(*packet)) ESP_LOGE(TAG, "Invalid PCM queue item");
      vRingbufferReturnItem(queue, packet);
      continue;
    }
    if (packet->rate != rate) {
      disable_output();
      i2s_std_clk_config_t clocks = I2S_STD_CLK_DEFAULT_CONFIG(packet->rate);
      clocks.mclk_multiple = I2S_MCLK_MULTIPLE_256;
      esp_err_t err = i2s_channel_reconfig_std_clock(tx, &clocks);
      if (err != ESP_OK) {
        atomic_fetch_add(&write_errors, 1);
        ESP_LOGE(TAG, "Clock configuration failed: %s", esp_err_to_name(err));
        vRingbufferReturnItem(queue, packet);
        rate = preloaded = 0;
        continue;
      }
      rate = packet->rate;
      preloaded = 0;
      ESP_LOGI(TAG, "Native PCM %lu Hz, BCK %lu Hz, MCLK %lu Hz",
               (unsigned long)rate, (unsigned long)dit4096_bclk(rate),
               (unsigned long)dit4096_mclk(rate));
    }
    last_packet_us = esp_timer_get_time();
    for (size_t i = 0; i < packet->frames * 2; ++i)
      words[i] = dit4096_pack_sample(packet->samples[i]);
    size_t bytes = packet->frames * 8, written = 0;
    esp_err_t err = ESP_OK;
    if (!atomic_load(&running)) {
      err = i2s_channel_preload_data(tx, words, bytes, &written);
      preloaded += written / 8;
      // A partial preload means the DMA ring is full, however it was rounded.
      if (err == ESP_OK && (written < bytes || preloaded >= DIT_PREBUFFER_FRAMES)) {
        // Reset stays low while clocks change and zero-padded PCM is preloaded.
        err = i2s_channel_enable(tx);
        if (err == ESP_OK) {
          atomic_store(&running, true);
          atomic_store(&sample_rate, rate);
          ESP_ERROR_CHECK(gpio_set_level(DIT_RESET_GPIO, 1));
          if (written < bytes) {
            size_t remaining_written = 0;
            err = i2s_channel_write(tx, (uint8_t *)words + written, bytes - written,
                                    &remaining_written, 100);
            written += remaining_written;
          }
        }
      }
    } else {
      int64_t started_us = esp_timer_get_time();
      err = i2s_channel_write(tx, words, bytes, &written, 100);
      uint32_t took_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
      if (took_ms > atomic_load(&max_write_ms)) atomic_store(&max_write_ms, took_ms);
    }
    if (err != ESP_OK || written != bytes) {
      atomic_fetch_add(&write_errors, 1);
      ESP_LOGE(TAG, "PCM DMA write failed: %s (%u/%u); rebuffering",
               esp_err_to_name(err), (unsigned)written, (unsigned)bytes);
      disable_output();
      rate = preloaded = 0;
    } else {
      atomic_fetch_add(&frames_submitted, written / 8);
    }
    vRingbufferReturnItem(queue, packet);
  }
}
#endif

esp_err_t dit4096_init(void) {
#ifdef DIT4096_ACTIVE
  ESP_RETURN_ON_ERROR(gpio_set_direction(DIT_RESET_GPIO, GPIO_MODE_OUTPUT), TAG, "Reset GPIO");
  ESP_RETURN_ON_ERROR(gpio_set_level(DIT_RESET_GPIO, 0), TAG, "Hold reset");
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  channel.dma_desc_num = DIT_DMA_COUNT;
  channel.dma_frame_num = DIT_DMA_FRAMES;
  channel.auto_clear = true;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &tx, NULL), TAG, "I2S1 allocation");
  i2s_std_config_t config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = GPIO_NUM_39, .bclk = GPIO_NUM_38, .ws = GPIO_NUM_15,
                   .dout = GPIO_NUM_16, .din = I2S_GPIO_UNUSED}};
  config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  config.slot_cfg.ws_width = 32;
  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &config), TAG, "PCM configuration");
  queue = xRingbufferCreateWithCaps(64 * 1024, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
  if (!queue) {
    ESP_LOGE(TAG, "Cannot allocate native PCM queue");
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreatePinnedToCore(dit_task, "dit_pcm", 4096, NULL, 8, NULL, 1) != pdPASS) {
    ESP_LOGE(TAG, "Cannot create native PCM task");
    return ESP_ERR_NO_MEM;
  }
  atomic_store(&enabled, true);
  ESP_LOGI(TAG, "Native DIT output ready; held in reset until PCM is buffered");
#endif
  return ESP_OK;
}

void dit4096_flush(void) {
#ifdef DIT4096_ACTIVE
  atomic_fetch_add(&generation, 1);
#endif
}

void dit4096_submit(const int16_t *pcm, size_t frames, uint32_t rate, int32_t gain_q15) {
#ifdef DIT4096_ACTIVE
  if (!atomic_load(&enabled)) return;
  if (!pcm || !frames || frames > DIT_FRAMES || !dit4096_rate_supported(rate) ||
      gain_q15 < 0 || gain_q15 > 32768) {
    ESP_LOGE(TAG, "Unsupported native PCM block: %u frames, %lu Hz, gain %ld",
             (unsigned)frames, (unsigned long)rate, (long)gain_q15);
    atomic_fetch_add(&write_errors, 1);
    dit4096_flush();
    return;
  }
  outgoing.generation = atomic_load(&generation);
  outgoing.rate = rate;
  outgoing.frames = frames;
  for (size_t i = 0; i < frames; ++i) {
    int32_t delta = gain_q15 - gain;
    if (delta) gain += delta / 256 ? delta / 256 : (delta > 0 ? 1 : -1);
    outgoing.samples[i * 2] = (int32_t)pcm[i * 2] * gain >> 15;
    outgoing.samples[i * 2 + 1] = (int32_t)pcm[i * 2 + 1] * gain >> 15;
  }
  if (xRingbufferSend(queue, &outgoing, sizeof(outgoing), 0) != pdTRUE) {
    atomic_fetch_add(&queue_overflows, 1);
    int64_t stalled_ms = (esp_timer_get_time() - atomic_load(&last_consume_us)) / 1000;
    atomic_store(&overflow_stall_ms, (uint32_t)stalled_ms);
    atomic_store(&overflow_running, atomic_load(&running));
    ESP_LOGE(TAG, "Native PCM queue full (consumer idle %lld ms, running %d); resetting DIT stream",
             (long long)stalled_ms, (int)atomic_load(&running));
    dit4096_flush();
  } else {
    uint32_t free_bytes = (uint32_t)xRingbufferGetCurFreeSize(queue);
    if (free_bytes < atomic_load(&queue_min_free)) atomic_store(&queue_min_free, free_bytes);
  }
#else
  (void)pcm;
  (void)frames;
  (void)rate;
  (void)gain_q15;
#endif
}

void dit4096_get_status(dit4096_status_t *status) {
  memset(status, 0, sizeof(*status));
#ifdef DIT4096_ACTIVE
  status->enabled = atomic_load(&enabled);
  status->running = atomic_load(&running);
  status->sample_rate = atomic_load(&sample_rate);
  status->write_errors = atomic_load(&write_errors);
  status->queue_overflows = atomic_load(&queue_overflows);
  status->starvation_resets = atomic_load(&starvation_resets);
  status->frames_submitted = atomic_load(&frames_submitted);
  uint32_t min_free = atomic_load(&queue_min_free);
  status->queue_min_free_bytes = min_free == UINT32_MAX ? 0 : min_free;
  status->max_write_ms = atomic_load(&max_write_ms);
  status->overflow_stall_ms = atomic_load(&overflow_stall_ms);
  status->overflow_running = atomic_load(&overflow_running);
#endif
}
