#include <driver/i2s.h>
#include <old_awei_inferencing.h>

// ====== INMP441 硬體接腳 ======
#define I2S_WS   42
#define I2S_SD   41
#define I2S_SCK  40
#define I2S_PORT I2S_NUM_0

// ====== 採樣與滑動視窗設定 (250ms 更新一次) ======
#define SAMPLE_RATE EI_CLASSIFIER_FREQUENCY               // 16000Hz
#define TOTAL_SAMPLES EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE // 16000點 (1000ms)
#define NEW_SAMPLES_COUNT (SAMPLE_RATE / 4)               // 250ms 的採樣點數 = 4000點

// 環形/環狀音訊快取區
float audio_buffer[TOTAL_SAMPLES] = {0};

// 讀取 I2S 聲音資料至全域 Buffer
void capture_new_samples() {
    // 舊資料向前平移 250ms
    memmove(&audio_buffer[0], &audio_buffer[NEW_SAMPLES_COUNT], (TOTAL_SAMPLES - NEW_SAMPLES_COUNT) * sizeof(float));

    // 填入最新的 250ms 音訊
    size_t bytes_read;
    int32_t raw_sample = 0;

    for (int i = TOTAL_SAMPLES - NEW_SAMPLES_COUNT; i < TOTAL_SAMPLES; i++) {
        i2s_read(I2S_PORT, &raw_sample, sizeof(raw_sample), &bytes_read, portMAX_DELAY);
        int32_t processed_sample = raw_sample >> 14; 
        audio_buffer[i] = (float)processed_sample;
    }
}

// Edge Impulse 資料回呼函式
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

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);

    run_classifier_init();
    Serial.println("==========================================");
    Serial.println(" ESP32-S3 實時語音狀態監控已啟動 (250ms) ");
    Serial.println("==========================================");
}

void loop() {
    // 1. 擷取最新 250ms 音訊
    capture_new_samples();

    // 2. 構建推論 Signal
    signal_t signal;
    signal.total_length = TOTAL_SAMPLES;
    signal.get_data = &get_audio_signal_data;

    // 3. 執行推論
    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);

    if (r != EI_IMPULSE_OK) return;

    // 4. 尋找當前信心度最高的分類標籤
    int max_idx = 0;
    float max_val = 0.0;
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        if (result.classification[ix].value > max_val) {
            max_val = result.classification[ix].value;
            max_idx = ix;
        }
    }

    // 5. 實時在 Serial 輸出當前狀態 (格式：[時間毫秒] 狀態: 類別名稱 | 信心度)
    Serial.printf("[%08lu ms] 當前狀態: %-20s | 信心度: %.2f", 
                  millis(), 
                  result.classification[max_idx].label, 
                  max_val);

    // 6. 關鍵字觸發判斷 (假設 index 1 為靠北/bad，門檻設為 0.90)
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    // strcmp 會比較兩段文字，若完全相同就會回傳 0
    if (strcmp(result.classification[ix].label, "carryfather") == 0) {
        if (result.classification[ix].value >= 0.95) {
            Serial.print("  <-- 🔥 偵測到關鍵字！");
        }
    }
}

    Serial.println(); // 換行
}