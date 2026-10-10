#include <driver/i2s.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <Wire.h>
#include <U8g2lib.h>

// Standalone microphone-input diagnostic for the YD-ESP32-S3 N16R8.
// It deliberately does not load Edge Impulse or run the classifier.
// Its only job is to prove whether audio arriving at the selected I2S input
// changes when
// someone talks to or taps the INMP441.

// INMP441 wiring:
//   SCK -> GPIO4, WS -> GPIO5, SD -> GPIO6, L/R -> GND
//   VDD -> 3V3, GND -> GND
#define I2S_SCK  4
#define I2S_WS   5
#define I2S_SD   6
#define I2S_PORT I2S_NUM_0

// OLED wiring:
//   SDA -> GPIO8, SCL/SCK -> GPIO9, VDD -> 3V3, GND -> GND
#define OLED_SDA 8
#define OLED_SCL 9

#define AUDIO_SAMPLE_RATE     16000
#define I2S_BATCH_SAMPLES       256
#define STATS_WINDOW_SAMPLES   4096
#define TRAINING_MIC_GAIN         4

// The photographed M130-12864-4G OLED is normally SH1106 based.
// If it initializes but remains blank, use
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C instead. Wiring is unchanged.
static U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

struct AudioStats {
    int32_t sample_min;
    int32_t sample_max;
    int32_t raw_min;
    int32_t raw_max;
    int64_t sample_sum;
    int64_t sample_sum_squares;
    uint32_t count;
};

static bool oled_ready = false;
static bool i2s_ready = false;
static uint8_t oled_address = 0;
static uint32_t frame_number = 0;
static AudioStats stats;

// This is the exact raw-to-WAV conversion used by the verified classifier:
// top 16 bits, gain x4, saturated to signed 16-bit range.
static int32_t training_wav_sample_from_raw(int32_t raw_sample) {
    int32_t sample = (raw_sample >> 16) * TRAINING_MIC_GAIN;
    if (sample > INT16_MAX) return INT16_MAX;
    if (sample < INT16_MIN) return INT16_MIN;
    return sample;
}

static void reset_stats() {
    stats.sample_min = INT32_MAX;
    stats.sample_max = INT32_MIN;
    stats.raw_min = INT32_MAX;
    stats.raw_max = INT32_MIN;
    stats.sample_sum = 0;
    stats.sample_sum_squares = 0;
    stats.count = 0;
}

static bool i2c_device_present(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

static void draw_lines(const char *line1, const char *line2,
                       const char *line3, const char *line4,
                       const char *line5) {
    if (!oled_ready) return;

    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 10, line1);
    oled.drawHLine(0, 13, 128);
    oled.drawStr(0, 25, line2);
    oled.drawStr(0, 36, line3);
    oled.drawStr(0, 47, line4);
    oled.setFont(u8g2_font_5x8_tf);
    oled.drawStr(0, 62, line5);
    oled.sendBuffer();
}

static bool init_oled() {
    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);

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
    Serial.printf("[OLED] SH1106 at 0x%02X; SDA=%d SCL=%d\n",
                  oled_address, OLED_SDA, OLED_SCL);
    return true;
}

static void render_i2s_problem(esp_err_t error, size_t bytes_read) {
    char line4[30];
    snprintf(line4, sizeof(line4), "err=%d bytes=%u", (int)error,
             (unsigned int)bytes_read);
    draw_lines("MIC INPUT PROBE", "I2S READ TIMEOUT/ERR",
               "check SCK=4 WS=5", line4, "SD=6, L/R must be GND");
    Serial.printf("[I2S] read error=%d bytes=%u\n", (int)error,
                  (unsigned int)bytes_read);
}

static void render_stats() {
    const double dc = (double)stats.sample_sum / (double)stats.count;
    const double mean_square =
        (double)stats.sample_sum_squares / (double)stats.count;
    double variance = mean_square - (dc * dc);
    if (variance < 0.0) variance = 0.0; // protect against floating rounding.

    const long dc_rounded = lround(dc);
    const unsigned long ac_rms = (unsigned long)lround(sqrt(variance));
    const unsigned long peak_to_peak =
        (unsigned long)(stats.sample_max - stats.sample_min);

    char frame_line[28];
    char p2p_line[28];
    char rms_line[28];
    char dc_line[28];
    snprintf(frame_line, sizeof(frame_line), "frame: %lu", (unsigned long)frame_number);
    snprintf(p2p_line, sizeof(p2p_line), "P2P: %lu", peak_to_peak);
    snprintf(rms_line, sizeof(rms_line), "AC RMS: %lu", ac_rms);
    snprintf(dc_line, sizeof(dc_line), "DC: %ld", dc_rounded);

    draw_lines("MIC INPUT PROBE", frame_line, p2p_line, rms_line, dc_line);

    Serial.printf(
        "[%08lu ms] n=%lu rawMin=%ld rawMax=%ld min=%ld max=%ld "
        "p2p=%lu dc=%ld acRms=%lu\n",
        millis(), (unsigned long)stats.count, (long)stats.raw_min,
        (long)stats.raw_max, (long)stats.sample_min, (long)stats.sample_max,
        peak_to_peak, dc_rounded, ac_rms);
}

static bool init_i2s() {
    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AUDIO_SAMPLE_RATE,
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

    const esp_err_t driver_result =
        i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    const esp_err_t pin_result = i2s_set_pin(I2S_PORT, &pin_config);
    if (driver_result != ESP_OK || pin_result != ESP_OK) {
        char line4[30];
        snprintf(line4, sizeof(line4), "driver=%d pins=%d",
                 (int)driver_result, (int)pin_result);
        draw_lines("MIC INPUT PROBE", "I2S CONFIG ERROR",
                   "SCK=4 WS=5 SD=6", line4, "power-cycle then retry");
        Serial.printf("[I2S] config error: driver=%d pins=%d\n",
                      (int)driver_result, (int)pin_result);
        return false;
    }
    return true;
}

void setup() {
    // Do not wait for Serial: native USB / UART selection can differ,
    // while the OLED must remain usable when the device is offline.
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println("[BOOT] N16R8 microphone-input probe");

    init_oled();
    draw_lines("MIC INPUT PROBE", "initializing...", "",
               "same gain as model", "talk or tap the mic");

    i2s_ready = init_i2s();
    if (i2s_ready) {
        reset_stats();
        draw_lines("MIC INPUT PROBE", "ready: talk or tap", "",
                   "watch P2P / RMS", "frame number increments");
        Serial.println("[BOOT] I2S RX ready: SCK=4 WS=5 SD=6; L/R=GND.");
    }
}

void loop() {
    if (!i2s_ready) {
        delay(500);
        return;
    }

    int32_t raw_batch[I2S_BATCH_SAMPLES];
    size_t bytes_read = 0;
    const esp_err_t read_result = i2s_read(
        I2S_PORT, raw_batch, sizeof(raw_batch), &bytes_read,
        pdMS_TO_TICKS(500));

    if (read_result != ESP_OK || bytes_read == 0) {
        render_i2s_problem(read_result, bytes_read);
        delay(50);
        return;
    }

    const size_t sample_count = bytes_read / sizeof(raw_batch[0]);
    for (size_t ix = 0; ix < sample_count; ix++) {
        const int32_t raw_sample = raw_batch[ix];
        const int32_t sample = training_wav_sample_from_raw(raw_sample);

        if (raw_sample < stats.raw_min) stats.raw_min = raw_sample;
        if (raw_sample > stats.raw_max) stats.raw_max = raw_sample;
        if (sample < stats.sample_min) stats.sample_min = sample;
        if (sample > stats.sample_max) stats.sample_max = sample;
        stats.sample_sum += sample;
        stats.sample_sum_squares += (int64_t)sample * (int64_t)sample;
        stats.count++;
    }

    if (stats.count >= STATS_WINDOW_SAMPLES) {
        frame_number++;
        render_stats();
        reset_stats();
    }
}
