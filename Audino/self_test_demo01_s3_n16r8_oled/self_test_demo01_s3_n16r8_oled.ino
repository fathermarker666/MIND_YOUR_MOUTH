#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <Wire.h>
#include <U8g2lib.h>

// This is a new, standalone N16R8 + OLED sketch.
// It deliberately keeps the verified TFLite input path used by
// self_test_demo01_diagnostics: (raw >> 16) * 4, with int16 saturation.
// Do not include the older EON library in this sketch.
#include <INMP441record_A_tflite_inferencing.h>

// YD-ESP32-S3 N16R8 wiring.
#define I2S_SCK   4   // INMP441 SCK; future I2S amplifier BCLK
#define I2S_WS    5   // INMP441 WS;  future I2S amplifier LRCLK
#define I2S_SD    6   // INMP441 SD -> ESP32-S3 data input
#define I2S_DOUT  7   // Reserved for future ESP32-S3 -> amplifier DIN
#define I2S_PORT  I2S_NUM_0

#define OLED_SDA  8
#define OLED_SCL  9

// The pictured M130-12864-4G module is a 128x64 I2C OLED and is normally
// SH1106-based. If the I2C scan succeeds but the panel stays blank, replace
// the next constructor with U8G2_SSD1306_128X64_NONAME_F_HW_I2C. The wiring
// remains identical.
static U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

#define SAMPLE_RATE             EI_CLASSIFIER_FREQUENCY
#define TOTAL_SAMPLES            EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE
#define NEW_SAMPLES_COUNT       (SAMPLE_RATE / 4)
#define I2S_BATCH_SAMPLES       256
#define RING_SAMPLES            (TOTAL_SAMPLES * 2)
#define SNAPSHOT_GUARD_SAMPLES  I2S_BATCH_SAMPLES
#define TRAINING_MIC_GAIN       4
#define BAD_EVENT_THRESHOLD     0.95f

static int32_t raw_ring[RING_SAMPLES] = {0};
static float audio_buffer[TOTAL_SAMPLES] = {0};

static SemaphoreHandle_t audio_mutex = NULL;
static TaskHandle_t capture_task_handle = NULL;
static size_t ring_head = 0;
static uint32_t ring_valid_samples = 0;
static uint32_t samples_written_total = 0;
static uint32_t last_window_end_sequence = 0;
static uint32_t windows_captured = 0;

static bool oled_ready = false;
static uint8_t oled_address = 0;

// Exact WAV conversion used by the training recorder and the verified
// TFLite diagnostic sketch: top 16 bits, gain 4, then signed-16-bit clipping.
static int32_t training_wav_sample_from_raw(int32_t raw_sample) {
    int32_t sample = (raw_sample >> 16) * TRAINING_MIC_GAIN;
    if (sample > INT16_MAX) return INT16_MAX;
    if (sample < INT16_MIN) return INT16_MIN;
    return sample;
}

// I2S remains on core 0 while inference and OLED rendering happen in loop()
// on the other Arduino core. Never use Wire or the OLED from this task.
static void i2s_capture_task(void *parameter) {
    (void)parameter;
    int32_t raw_batch[I2S_BATCH_SAMPLES];

    for (;;) {
        size_t bytes_read = 0;
        const esp_err_t read_result = i2s_read(
            I2S_PORT, raw_batch, sizeof(raw_batch), &bytes_read, portMAX_DELAY);
        const size_t sample_count = bytes_read / sizeof(raw_batch[0]);

        xSemaphoreTake(audio_mutex, portMAX_DELAY);
        if (read_result == ESP_OK) {
            for (size_t ix = 0; ix < sample_count; ix++) {
                raw_ring[ring_head] = raw_batch[ix];
                ring_head = (ring_head + 1) % RING_SAMPLES;
                if (ring_valid_samples < RING_SAMPLES) ring_valid_samples++;
                samples_written_total++;
            }
        }
        xSemaphoreGive(audio_mutex);
    }
}

// Take a stable, one-second window every 250 ms. The 16 ms guard means the
// capture task cannot overwrite data while this copy is being performed.
static bool snapshot_latest_window() {
    uint32_t snapshot_end_sequence = 0;

    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    if (ring_valid_samples < TOTAL_SAMPLES + SNAPSHOT_GUARD_SAMPLES ||
        (windows_captured > 0 &&
         (uint32_t)(samples_written_total - SNAPSHOT_GUARD_SAMPLES -
                    last_window_end_sequence) < NEW_SAMPLES_COUNT)) {
        xSemaphoreGive(audio_mutex);
        return false;
    }
    snapshot_end_sequence = samples_written_total - SNAPSHOT_GUARD_SAMPLES;
    xSemaphoreGive(audio_mutex);

    size_t read_index = (snapshot_end_sequence - TOTAL_SAMPLES) % RING_SAMPLES;
    for (size_t ix = 0; ix < TOTAL_SAMPLES; ix++) {
        audio_buffer[ix] = (float)training_wav_sample_from_raw(raw_ring[read_index]);
        read_index = (read_index + 1) % RING_SAMPLES;
    }

    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    last_window_end_sequence = snapshot_end_sequence;
    xSemaphoreGive(audio_mutex);
    windows_captured++;
    return true;
}

static int get_audio_signal_data(size_t offset, size_t length, float *out_ptr) {
    if (offset + length > TOTAL_SAMPLES) return -1;
    memcpy(out_ptr, &audio_buffer[offset], length * sizeof(float));
    return 0;
}

static bool i2c_device_present(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

static void oled_render(const char *label, float confidence, const char *footer) {
    if (!oled_ready) return;

    char score_text[24];
    if (confidence >= 0.0f) {
        snprintf(score_text, sizeof(score_text), "confidence: %.1f%%",
                 (double)(confidence * 100.0f));
    } else {
        snprintf(score_text, sizeof(score_text), "confidence: --");
    }

    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tf);
    oled.drawStr(0, 11, "MIND YOUR MOUTH");
    oled.drawHLine(0, 14, 128);

    oled.setFont(u8g2_font_logisoso16_tf);
    const int16_t label_x = (128 - oled.getStrWidth(label)) / 2;
    oled.drawStr(label_x < 0 ? 0 : label_x, 40, label);

    oled.setFont(u8g2_font_6x12_tf);
    oled.drawStr(0, 53, score_text);
    oled.setFont(u8g2_font_5x8_tf);
    oled.drawStr(0, 63, footer);
    oled.sendBuffer();
}

static bool init_oled() {
    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);

    // Both addresses are common for this type of OLED. The detected address
    // is printed so a wiring or module variant is obvious immediately.
    const uint8_t candidates[] = {0x3C, 0x3D};
    for (size_t ix = 0; ix < sizeof(candidates); ix++) {
        if (i2c_device_present(candidates[ix])) {
            oled_address = candidates[ix];
            break;
        }
    }

    if (oled_address == 0) {
        Serial.println("[OLED] No response at 0x3C or 0x3D.");
        return false;
    }

    oled.setI2CAddress(oled_address << 1);
    oled.setBusClock(400000);
    oled.begin();
    oled_ready = true;
    Serial.printf("[OLED] SH1106 init at I2C address 0x%02X on SDA=%d SCL=%d\n",
                  oled_address, OLED_SDA, OLED_SCL);
    oled_render("BOOT", -1.0f, "OLED READY");
    return true;
}

static void show_classifier_status(const ei_impulse_result_t *result) {
    size_t top_index = 0;
    for (size_t ix = 1; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        if (result->classification[ix].value >
            result->classification[top_index].value) {
            top_index = ix;
        }
    }

    const char *label = result->classification[top_index].label;
    const float confidence = result->classification[top_index].value;
    const bool bad_event = strcmp(label, "BAD") == 0 &&
                           confidence >= BAD_EVENT_THRESHOLD;
    oled_render(label, confidence, bad_event ? "BAD EVENT >= 0.95" : "RAW STATUS");

    Serial.printf("[%08lu ms] %s=%.5f (DSP=%lu ms, classify=%lu ms)%s\n",
                  millis(), label, confidence,
                  (unsigned long)result->timing.dsp,
                  (unsigned long)result->timing.classification,
                  bad_event ? "  EVENT: BAD >= 0.95" : "");
}

void setup() {
    Serial.begin(115200);
    const uint32_t serial_wait_started = millis();
    while (!Serial && (millis() - serial_wait_started) < 3000) delay(10);
    Serial.println();
    Serial.println("[BOOT] N16R8 TFLite + OLED status firmware.");

    init_oled();

    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        // GPIO7 is deliberately not active until an I2S amplifier is fitted.
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };

    const esp_err_t driver_result = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    const esp_err_t pin_result = i2s_set_pin(I2S_PORT, &pin_config);
    audio_mutex = xSemaphoreCreateMutex();
    if (driver_result != ESP_OK || pin_result != ESP_OK || audio_mutex == NULL) {
        Serial.printf("[BOOT] I2S error: driver=%d pins=%d mutex=%s\n",
                      (int)driver_result, (int)pin_result,
                      audio_mutex ? "OK" : "FAIL");
        oled_render("ERROR", -1.0f, "I2S SETUP FAILED");
        return;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(
        i2s_capture_task, "i2s_capture", 4096, NULL, 1,
        &capture_task_handle, 0);
    if (task_result != pdPASS) {
        Serial.println("[BOOT] I2S capture task failed.");
        oled_render("ERROR", -1.0f, "CAPTURE TASK FAILED");
        return;
    }

    run_classifier_init();
    Serial.printf("[BOOT] Model: %lu Hz, %lu samples; map=(raw >> 16) * %d\n",
                  (unsigned long)SAMPLE_RATE, (unsigned long)TOTAL_SAMPLES,
                  TRAINING_MIC_GAIN);
    Serial.printf("[BOOT] I2S: BCLK=%d WS=%d DIN=%d; OLED: SDA=%d SCL=%d\n",
                  I2S_SCK, I2S_WS, I2S_SD, OLED_SDA, OLED_SCL);
    oled_render("LISTENING", -1.0f, "WAITING FOR 1 SEC WINDOW");
}

void loop() {
    if (!snapshot_latest_window()) {
        delay(5);
        return;
    }

    signal_t signal;
    signal.total_length = TOTAL_SAMPLES;
    signal.get_data = &get_audio_signal_data;
    ei_impulse_result_t result = {0};
    const EI_IMPULSE_ERROR run_result = run_classifier(&signal, &result, false);
    if (run_result != EI_IMPULSE_OK) {
        Serial.printf("[EI] run_classifier error=%d\n", (int)run_result);
        oled_render("ERROR", -1.0f, "CLASSIFIER FAILED");
        delay(50);
        return;
    }

    show_classifier_status(&result);
}
