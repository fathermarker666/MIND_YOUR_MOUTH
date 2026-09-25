#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <string.h>
#include <INMP441record_A_tflite_inferencing.h>

// Safe diagnostic copy of self_test_demo01.ino.
// Keeps the original model and I2S pins/settings. Capture uses a background
// task and a continuous ring; this A/B build maps samples exactly as the
// G4 training WAV recorder does: (raw >> 16) * 4 with int16 saturation.

#define I2S_WS   42
#define I2S_SD   41
#define I2S_SCK  40
#define I2S_PORT I2S_NUM_0

#define SAMPLE_RATE       EI_CLASSIFIER_FREQUENCY
#define TOTAL_SAMPLES     EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE
#define NEW_SAMPLES_COUNT (SAMPLE_RATE / 4)
#define I2S_BATCH_SAMPLES 256
#define RING_SAMPLES       (TOTAL_SAMPLES * 2)
#define SNAPSHOT_GUARD_SAMPLES I2S_BATCH_SAMPLES
#define TRAINING_MIC_GAIN 4

// Producer and consumer are separated: I2S never writes into the data passed
// to Edge Impulse. Two seconds of history means a one-second window selected
// 16 ms in the past remains immutable while it is copied for inference.
static int32_t raw_ring[RING_SAMPLES] = {0};
static float audio_buffer[TOTAL_SAMPLES] = {0};

static SemaphoreHandle_t audio_mutex = NULL;
static TaskHandle_t capture_task_handle = NULL;
static size_t ring_head = 0; // Next raw_ring write position.
static uint32_t ring_valid_samples = 0;
static uint32_t samples_written_total = 0;
static uint32_t total_short_reads = 0;
static uint32_t total_i2s_errors = 0;

struct SampleStats {
    int32_t min_value;
    int32_t max_value;
    double sum_squares;
    uint32_t count;
};

struct CaptureDiagnostics {
    SampleStats raw;
    SampleStats converted;
    uint32_t short_reads_since_last_window;
    uint32_t i2s_errors_since_last_window;
    uint32_t total_short_reads;
    uint32_t total_i2s_errors;
    uint32_t copy_us;
    uint32_t window_end_sequence;
};

static CaptureDiagnostics last_capture = {};
static uint32_t windows_captured = 0;
static uint32_t last_window_end_sequence = 0;
static uint32_t last_reported_short_reads = 0;
static uint32_t last_reported_i2s_errors = 0;
static bool latest_window_available = false;
static bool dump_requested = false;
static bool future_dump_armed = false;
static uint32_t future_dump_end_sequence = 0;
static char serial_command[16] = {0};
static size_t serial_command_length = 0;

static void reset_stats(SampleStats *stats) {
    stats->min_value = INT32_MAX;
    stats->max_value = INT32_MIN;
    stats->sum_squares = 0.0;
    stats->count = 0;
}

static void update_stats(SampleStats *stats, int32_t value) {
    if (value < stats->min_value) stats->min_value = value;
    if (value > stats->max_value) stats->max_value = value;
    stats->sum_squares += (double)value * (double)value;
    stats->count++;
}

static double stats_rms(const SampleStats *stats) {
    return stats->count == 0 ? 0.0 : sqrt(stats->sum_squares / (double)stats->count);
}

static int32_t stats_peak(const SampleStats *stats) {
    int64_t low = -(int64_t)stats->min_value;
    int64_t high = stats->max_value;
    return (int32_t)(low > high ? low : high);
}

// Exact conversion used by the G4 training recorder before it writes a WAV:
// take the upper 16 bits, apply gain 4, then saturate to signed 16-bit.
static int32_t training_wav_sample_from_raw(int32_t raw_sample) {
    int32_t sample = (raw_sample >> 16) * TRAINING_MIC_GAIN;
    if (sample > INT16_MAX) return INT16_MAX;
    if (sample < INT16_MIN) return INT16_MIN;
    return sample;
}

// Every read is 256 samples = 16 ms at 16 kHz. This keeps running during the
// approximately 100 ms DSP call, preventing the I2S DMA queue from going stale.
static void i2s_capture_task(void *parameter) {
    (void)parameter;
    int32_t raw_batch[I2S_BATCH_SAMPLES];

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t read_result = i2s_read(
            I2S_PORT, raw_batch, sizeof(raw_batch), &bytes_read, portMAX_DELAY);
        const size_t sample_count = bytes_read / sizeof(raw_batch[0]);

        xSemaphoreTake(audio_mutex, portMAX_DELAY);
        if (read_result != ESP_OK) total_i2s_errors++;
        if (bytes_read != sizeof(raw_batch)) total_short_reads++;

        for (size_t ix = 0; ix < sample_count; ix++) {
            raw_ring[ring_head] = raw_batch[ix];
            ring_head = (ring_head + 1) % RING_SAMPLES;
            if (ring_valid_samples < RING_SAMPLES) ring_valid_samples++;
            samples_written_total++;
        }
        xSemaphoreGive(audio_mutex);
    }
}

// Snapshot a coherent 1-second window without holding up I2S while copying.
// The end point is one batch (16 ms) behind the writer. With two seconds in
// the ring, it cannot be overwritten during this copy.
static bool snapshot_latest_window() {
    const uint32_t started_at = micros();
    reset_stats(&last_capture.raw);
    reset_stats(&last_capture.converted);

    uint32_t snapshot_end_sequence = 0;
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    if (ring_valid_samples < TOTAL_SAMPLES + SNAPSHOT_GUARD_SAMPLES ||
        (windows_captured > 0 &&
         (uint32_t)(samples_written_total - SNAPSHOT_GUARD_SAMPLES - last_window_end_sequence) < NEW_SAMPLES_COUNT)) {
        xSemaphoreGive(audio_mutex);
        return false;
    }
    snapshot_end_sequence = samples_written_total - SNAPSHOT_GUARD_SAMPLES;
    xSemaphoreGive(audio_mutex);

    // `samples_written_total` is one-based relative to the last written item.
    // Start at the first item of the selected chronological 1-second window.
    size_t read_index = (snapshot_end_sequence - TOTAL_SAMPLES) % RING_SAMPLES;
    for (size_t ix = 0; ix < TOTAL_SAMPLES; ix++) {
        const int32_t raw_sample = raw_ring[read_index];
        const int32_t processed_sample = training_wav_sample_from_raw(raw_sample);
        audio_buffer[ix] = (float)processed_sample;
        update_stats(&last_capture.raw, raw_sample);
        update_stats(&last_capture.converted, processed_sample);
        read_index = (read_index + 1) % RING_SAMPLES;
    }

    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    last_capture.short_reads_since_last_window = total_short_reads - last_reported_short_reads;
    last_capture.i2s_errors_since_last_window = total_i2s_errors - last_reported_i2s_errors;
    last_capture.total_short_reads = total_short_reads;
    last_capture.total_i2s_errors = total_i2s_errors;
    last_reported_short_reads = total_short_reads;
    last_reported_i2s_errors = total_i2s_errors;
    last_window_end_sequence = snapshot_end_sequence;
    last_capture.window_end_sequence = snapshot_end_sequence;
    xSemaphoreGive(audio_mutex);

    last_capture.copy_us = micros() - started_at;
    windows_captured++;
    latest_window_available = true;
    return true;
}

// `REC 1` captures the *next* complete second, rather than returning audio
// that happened before the PC button was pressed. This lets a person press
// Record and speak immediately; no manual timing is required.
static bool snapshot_armed_future_window() {
    uint32_t snapshot_end_sequence = 0;
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    if (!future_dump_armed ||
        samples_written_total < future_dump_end_sequence + SNAPSHOT_GUARD_SAMPLES) {
        xSemaphoreGive(audio_mutex);
        return false;
    }
    snapshot_end_sequence = future_dump_end_sequence;
    xSemaphoreGive(audio_mutex);

    const uint32_t started_at = micros();
    reset_stats(&last_capture.raw);
    reset_stats(&last_capture.converted);
    size_t read_index = (snapshot_end_sequence - TOTAL_SAMPLES) % RING_SAMPLES;
    for (size_t ix = 0; ix < TOTAL_SAMPLES; ix++) {
        const int32_t raw_sample = raw_ring[read_index];
        const int32_t processed_sample = training_wav_sample_from_raw(raw_sample);
        audio_buffer[ix] = (float)processed_sample;
        update_stats(&last_capture.raw, raw_sample);
        update_stats(&last_capture.converted, processed_sample);
        read_index = (read_index + 1) % RING_SAMPLES;
    }

    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    last_capture.short_reads_since_last_window = total_short_reads - last_reported_short_reads;
    last_capture.i2s_errors_since_last_window = total_i2s_errors - last_reported_i2s_errors;
    last_capture.total_short_reads = total_short_reads;
    last_capture.total_i2s_errors = total_i2s_errors;
    last_reported_short_reads = total_short_reads;
    last_reported_i2s_errors = total_i2s_errors;
    last_window_end_sequence = snapshot_end_sequence;
    last_capture.window_end_sequence = snapshot_end_sequence;
    future_dump_armed = false;
    xSemaphoreGive(audio_mutex);

    last_capture.copy_us = micros() - started_at;
    windows_captured++;
    latest_window_available = true;
    return true;
}

static int get_audio_signal_data(size_t offset, size_t length, float *out_ptr) {
    memcpy(out_ptr, &audio_buffer[offset], length * sizeof(float));
    return 0;
}

static void print_stats(const char *name, const SampleStats *stats) {
    Serial.printf("  %s: n=%lu min=%ld max=%ld peak=%ld rms=%.1f\n",
                  name, (unsigned long)stats->count, (long)stats->min_value,
                  (long)stats->max_value, (long)stats_peak(stats), stats_rms(stats));
}

static void write_wav_header(uint32_t pcm_bytes) {
    const uint32_t file_size = 36 + pcm_bytes;
    const uint32_t fmt_size = 16;
    const uint16_t audio_format = 1;
    const uint16_t channels = 1;
    const uint32_t sample_rate = SAMPLE_RATE;
    const uint32_t byte_rate = sample_rate * channels * sizeof(int16_t);
    const uint16_t block_align = channels * sizeof(int16_t);
    const uint16_t bits_per_sample = 16;

    Serial.write("RIFF", 4);
    Serial.write((const uint8_t *)&file_size, sizeof(file_size));
    Serial.write("WAVE", 4);
    Serial.write("fmt ", 4);
    Serial.write((const uint8_t *)&fmt_size, sizeof(fmt_size));
    Serial.write((const uint8_t *)&audio_format, sizeof(audio_format));
    Serial.write((const uint8_t *)&channels, sizeof(channels));
    Serial.write((const uint8_t *)&sample_rate, sizeof(sample_rate));
    Serial.write((const uint8_t *)&byte_rate, sizeof(byte_rate));
    Serial.write((const uint8_t *)&block_align, sizeof(block_align));
    Serial.write((const uint8_t *)&bits_per_sample, sizeof(bits_per_sample));
    Serial.write("data", 4);
    Serial.write((const uint8_t *)&pcm_bytes, sizeof(pcm_bytes));
}

// This is deliberately called only after a REC 1 WAV transfer. It runs the
// embedded int8 model on the exact audio_buffer that was just written to the
// WAV file, eliminating any uncertainty about timing or conversion.
static void print_same_wav_on_device_result() {
    signal_t signal;
    signal.total_length = TOTAL_SAMPLES;
    signal.get_data = &get_audio_signal_data;
    ei_impulse_result_t result = {0};
    const EI_IMPULSE_ERROR run_result = run_classifier(&signal, &result, false);

    Serial.println("[SAME_WAV_ON_DEVICE] exact exported buffer:");
    if (run_result != EI_IMPULSE_OK) {
        Serial.printf("  ERROR: run_classifier=%d\n", (int)run_result);
        return;
    }
    Serial.printf("  timing: DSP=%lu ms, classification=%lu ms\n",
                  (unsigned long)result.timing.dsp,
                  (unsigned long)result.timing.classification);
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        Serial.printf("  %-16s %.5f\n", result.classification[ix].label,
                      result.classification[ix].value);
    }
}

// Sends the exact, already-converted one-second buffer given to Edge Impulse.
// The protocol deliberately matches the existing PC recorder: REC 1 -> WAV.
static void dump_latest_inference_wav() {
    if (!latest_window_available) {
        Serial.println("[DUMP] No complete one-second window yet.");
        return;
    }

    const uint32_t pcm_bytes = TOTAL_SAMPLES * sizeof(int16_t);
    const uint32_t wav_bytes = 44 + pcm_bytes;
    Serial.printf("WAV_BEGIN %lu\n", (unsigned long)wav_bytes);
    delay(20);
    write_wav_header(pcm_bytes);

    for (size_t ix = 0; ix < TOTAL_SAMPLES; ix++) {
        int32_t sample = (int32_t)audio_buffer[ix];
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
        const int16_t pcm_sample = (int16_t)sample;
        Serial.write((const uint8_t *)&pcm_sample, sizeof(pcm_sample));
    }

    Serial.flush();
    Serial.println("WAV_END");
    print_same_wav_on_device_result();
    Serial.flush();
}

static void arm_future_dump() {
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    future_dump_end_sequence = samples_written_total + TOTAL_SAMPLES;
    future_dump_armed = true;
    xSemaphoreGive(audio_mutex);
    Serial.println("[DUMP] Recording the next complete one-second window.");
}

// D or R exports the latest window. `REC 1` records the next second, so a
// desktop button can behave like a normal immediate recorder.
static void poll_serial_commands() {
    while (Serial.available() > 0) {
        const char character = (char)Serial.read();
        if (character == '\r' || character == '\n') {
            if (serial_command_length > 0) {
                serial_command[serial_command_length] = '\0';
                if (strcmp(serial_command, "D") == 0 ||
                    strcmp(serial_command, "d") == 0 ||
                    strcmp(serial_command, "R") == 0 ||
                    strcmp(serial_command, "r") == 0) {
                    dump_requested = true;
                } else if (strcmp(serial_command, "REC 1") == 0) {
                    arm_future_dump();
                } else {
                    Serial.println("[DUMP] Use D for the latest WAV or REC 1 to record the next second.");
                }
            }
            serial_command_length = 0;
            continue;
        }

        if (serial_command_length < sizeof(serial_command) - 1) {
            serial_command[serial_command_length++] = character;
        }
    }
}

void setup() {
    Serial.begin(115200);
    const uint32_t serial_wait_started = millis();
    while (!Serial && (millis() - serial_wait_started) < 3000) delay(10);
    Serial.println();
    Serial.println("[BOOT] Continuous-capture diagnostics setup.");

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
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };

    const esp_err_t driver_result = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    const esp_err_t pin_result = i2s_set_pin(I2S_PORT, &pin_config);
    audio_mutex = xSemaphoreCreateMutex();
    Serial.printf("[BOOT] I2S driver_install=%d, set_pin=%d, mutex=%s\n",
                  (int)driver_result, (int)pin_result, audio_mutex ? "OK" : "FAIL");
    if (driver_result != ESP_OK || pin_result != ESP_OK || audio_mutex == NULL) {
        Serial.println("[BOOT] Fatal setup error; capture task not started.");
        return;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(
        i2s_capture_task, "i2s_capture", 4096, NULL, 1, &capture_task_handle, 0);
    Serial.printf("[BOOT] I2S capture task=%s (core 0; batch=%d; ring=%lu samples)\n",
                  task_result == pdPASS ? "OK" : "FAIL", I2S_BATCH_SAMPLES,
                  (unsigned long)RING_SAMPLES);
    if (task_result != pdPASS) return;

    run_classifier_init();
    Serial.println("==========================================");
    Serial.println("ESP32-S3 continuous capture diagnostics started");
    Serial.printf("Model: %lu Hz, %lu samples; mapping: (raw >> 16) * %d, int16 clip\n",
                  (unsigned long)SAMPLE_RATE, (unsigned long)TOTAL_SAMPLES,
                  TRAINING_MIC_GAIN);
    Serial.println("Wait about 1 second for the first complete window.");
    Serial.println("==========================================");
}

void loop() {
    poll_serial_commands();

    if (future_dump_armed) {
        if (snapshot_armed_future_window()) {
            dump_latest_inference_wav();
        }
        delay(1);
        return;
    }

    if (!snapshot_latest_window()) {
        delay(5);
        return;
    }

    if (dump_requested) {
        dump_requested = false;
        dump_latest_inference_wav();
        return;
    }

    signal_t signal;
    signal.total_length = TOTAL_SAMPLES;
    signal.get_data = &get_audio_signal_data;
    ei_impulse_result_t result = {0};
    const EI_IMPULSE_ERROR run_result = run_classifier(&signal, &result, false);

    Serial.printf("\n[%08lu ms] window=%lu continuous snapshot=%.2f ms end_sample=%lu\n",
                  millis(), (unsigned long)windows_captured,
                  (double)last_capture.copy_us / 1000.0,
                  (unsigned long)last_capture.window_end_sequence);
    print_stats("raw 32-bit", &last_capture.raw);
    print_stats("EI input (training WAV map)", &last_capture.converted);
    Serial.printf("  I2S: short_reads=%lu (total=%lu), errors=%lu (total=%lu)\n",
                  (unsigned long)last_capture.short_reads_since_last_window,
                  (unsigned long)last_capture.total_short_reads,
                  (unsigned long)last_capture.i2s_errors_since_last_window,
                  (unsigned long)last_capture.total_i2s_errors);
    if (run_result != EI_IMPULSE_OK) {
        Serial.printf("  ERROR: run_classifier=%d\n", (int)run_result);
        return;
    }

    Serial.printf("  timing: DSP=%lu ms, classification=%lu ms, anomaly=%lu ms\n",
                  (unsigned long)result.timing.dsp,
                  (unsigned long)result.timing.classification,
                  (unsigned long)result.timing.anomaly);
    Serial.println("  classifications:");
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        Serial.printf("    %-16s %.5f\n", result.classification[ix].label,
                      result.classification[ix].value);
    }
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        if (strcmp(result.classification[ix].label, "BAD") == 0 &&
            result.classification[ix].value >= 0.95f) {
            Serial.println("  EVENT: BAD >= 0.95");
        }
    }
}
