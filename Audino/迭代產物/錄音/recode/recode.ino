#include <Arduino.h>
#include <driver/i2s.h>

#define I2S_WS   42
#define I2S_SD   41
#define I2S_SCK  40
#define I2S_PORT I2S_NUM_0

#define SAMPLE_RATE 16000
#define RECORD_SECONDS 1
#define TOTAL_SAMPLES (SAMPLE_RATE * RECORD_SECONDS)

void setup() {
    Serial.begin(115200);
    while (!Serial) {
        delay(10);
    }

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

    // 清掉啟動時 DMA 裡可能殘留的資料
    i2s_zero_dma_buffer(I2S_PORT);

    Serial.println("ESP32-S3 INMP441 WAV Recorder Ready");
    Serial.println("Send 'R' to record.");
}

void writeWavHeader(uint32_t dataSize) {
    uint32_t fileSize = 36 + dataSize;
    uint16_t audioFormat = 1;       // PCM
    uint16_t channels = 1;          // Mono
    uint32_t sampleRate = SAMPLE_RATE;
    uint16_t bitsPerSample = 16;
    uint16_t blockAlign = channels * bitsPerSample / 8;
    uint32_t byteRate = sampleRate * blockAlign;

    Serial.write("RIFF", 4);
    Serial.write((uint8_t*)&fileSize, 4);
    Serial.write("WAVE", 4);

    Serial.write("fmt ", 4);

    uint32_t fmtSize = 16;
    Serial.write((uint8_t*)&fmtSize, 4);
    Serial.write((uint8_t*)&audioFormat, 2);
    Serial.write((uint8_t*)&channels, 2);
    Serial.write((uint8_t*)&sampleRate, 4);
    Serial.write((uint8_t*)&byteRate, 4);
    Serial.write((uint8_t*)&blockAlign, 2);
    Serial.write((uint8_t*)&bitsPerSample, 2);

    Serial.write("data", 4);
    Serial.write((uint8_t*)&dataSize, 4);
}

void recordWav() {

    const uint32_t dataSize = TOTAL_SAMPLES * sizeof(int16_t);

    // 告訴 PC：接下來就是 WAV binary
    Serial.println("WAV_BEGIN");

    delay(50);

    // WAV header
    writeWavHeader(dataSize);

    int32_t raw_sample;
    int16_t pcm_sample;

    size_t bytes_read;

    for (int i = 0; i < TOTAL_SAMPLES; i++) {

        i2s_read(
            I2S_PORT,
            &raw_sample,
            sizeof(raw_sample),
            &bytes_read,
            portMAX_DELAY
        );

        // INMP441 24-bit audio 放在 32-bit container
        // 轉成 16-bit PCM
        pcm_sample = (int16_t)(raw_sample >> 14);

        Serial.write(
            (uint8_t*)&pcm_sample,
            sizeof(pcm_sample)
        );
    }

    Serial.flush();

    Serial.println();
    Serial.println("WAV_END");
}

void loop() {

    if (Serial.available()) {

        char command = Serial.read();

        if (command == 'R' || command == 'r') {
            recordWav();
        }
    }
}
