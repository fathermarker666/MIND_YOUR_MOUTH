#include <driver/i2s.h>
#include <old_awei_inferencing.h> // 注意：如果你的 SDK 名稱已改為 people_man，請自行更換標頭檔

#define I2S_WS   42
#define I2S_SD   41
#define I2S_SCK  40
#define I2S_PORT I2S_NUM_0

#define SAMPLE_RATE EI_CLASSIFIER_FREQUENCY               // 16000Hz
#define TOTAL_SAMPLES EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE // 16000點 (1000ms)
#define NEW_SAMPLES_COUNT (SAMPLE_RATE / 4)               // 250ms = 4000點

float audio_buffer[TOTAL_SAMPLES] = {0};

// 暫存 DMA 批次讀取的原始 32-bit 數據緩衝區
int32_t raw_i2s_buffer[NEW_SAMPLES_COUNT];

// 軟體放大倍數 (請根據實際狀況微調，如果還是太小聲可以改為 8.0f 或 10.0f)
#define AUDIO_GAIN 4.0f 

void capture_new_samples() {
    // 1. 舊資料向前平移 250ms
    memmove(&audio_buffer[0], &audio_buffer[NEW_SAMPLES_COUNT], (TOTAL_SAMPLES - NEW_SAMPLES_COUNT) * sizeof(float));

    // 2. 使用 DMA 批次讀取 4000 個點 (極重要：維持 16kHz 時間軸精準)
    size_t bytes_read = 0;
    i2s_read(I2S_PORT, raw_i2s_buffer, sizeof(raw_i2s_buffer), &bytes_read, portMAX_DELAY);

    int samples_read = bytes_read / sizeof(int32_t);

    // 3. 轉成 float 並乘以 Gain 補償 Python 錄音時的音量差
    for (int i = 0; i < samples_read; i++) {
        int32_t sample = raw_i2s_buffer[i] >> 14;
        float scaled_sample = (float)sample * AUDIO_GAIN;

        // 限制在 16-bit 範圍 (-32768 ~ 32767) 防爆音
        if (scaled_sample > 32767.0f) scaled_sample = 32767.0f;
        if (scaled_sample < -32768.0f) scaled_sample = -32768.0f;

        audio_buffer[(TOTAL_SAMPLES - NEW_SAMPLES_COUNT) + i] = scaled_sample;
    }
}

static int get_audio_signal_data(size_t offset, size_t length, float *out_ptr) {
    memcpy(out_ptr, &audio_buffer[offset], length * sizeof(float));
    return 0;
}

void setup() {
    Serial.begin(115200);
    while (!Serial);

    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512, // 增大 DMA buffer 避免漏點
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

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);

    run_classifier_init();
    Serial.println("==========================================");
    Serial.println(" ESP32-S3 實時語音狀態監控已啟動 (DMA 優化版) ");
    Serial.println("==========================================");
}

void loop() {
    capture_new_samples();

    signal_t signal;
    signal.total_length = TOTAL_SAMPLES;
    signal.get_data = &get_audio_signal_data;

    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);

    if (r != EI_IMPULSE_OK) return;

    int max_idx = 0;
    float max_val = 0.0;
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        if (result.classification[ix].value > max_val) {
            max_val = result.classification[ix].value;
            max_idx = ix;
        }
    }

    Serial.printf("[%08lu ms] 狀態: %-15s | 信心度: %.2f\n", 
                  millis(), 
                  result.classification[max_idx].label, 
                  max_val);
}
