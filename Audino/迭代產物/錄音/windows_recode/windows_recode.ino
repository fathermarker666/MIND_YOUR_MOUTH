#include <Arduino.h>
#include <driver/i2s.h>

#define I2S_WS   42
#define I2S_SD   41
#define I2S_SCK  40
#define I2S_PORT I2S_NUM_0

#define SAMPLE_RATE     16000
#define RECORD_SECONDS  1
#define TOTAL_SAMPLES   (SAMPLE_RATE * RECORD_SECONDS)
#define PCM_BYTES       (TOTAL_SAMPLES * sizeof(int16_t))

void writeWavHeader(uint32_t dataSize) {
  uint32_t fileSize = 36 + dataSize;
  uint32_t fmtSize = 16;
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t audioFormat = 1;
  uint16_t channels = 1;
  uint16_t bitsPerSample = 16;
  uint16_t blockAlign = channels * bitsPerSample / 8;
  uint32_t byteRate = SAMPLE_RATE * blockAlign;

  Serial.write("RIFF", 4);
  Serial.write((uint8_t *)&fileSize, 4);
  Serial.write("WAVE", 4);
  Serial.write("fmt ", 4);
  Serial.write((uint8_t *)&fmtSize, 4);
  Serial.write((uint8_t *)&audioFormat, 2);
  Serial.write((uint8_t *)&channels, 2);
  Serial.write((uint8_t *)&sampleRate, 4);
  Serial.write((uint8_t *)&byteRate, 4);
  Serial.write((uint8_t *)&blockAlign, 2);
  Serial.write((uint8_t *)&bitsPerSample, 2);
  Serial.write("data", 4);
  Serial.write((uint8_t *)&dataSize, 4);
}

void recordAndSendWav() {
  // 這行是唯一出現在 WAV 前的文字訊息
  Serial.println("WAV_BEGIN");
  delay(30);

  writeWavHeader(PCM_BYTES);

  int32_t rawSample;
  int16_t pcmSample;
  size_t bytesRead;

  for (int i = 0; i < TOTAL_SAMPLES; i++) {
    i2s_read(
      I2S_PORT,
      &rawSample,
      sizeof(rawSample),
      &bytesRead,
      portMAX_DELAY
    );

    // 將 INMP441 的 32-bit I2S 資料轉成 16-bit PCM
    pcmSample = (int16_t)(rawSample >> 16);

    Serial.write((uint8_t *)&pcmSample, sizeof(pcmSample));
  }

  Serial.flush();

  // 這行一定在固定的 32044 bytes 後面
  Serial.println("WAV_END");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  const i2s_config_t i2sConfig = {
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

  const i2s_pin_config_t pinConfig = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2sConfig, 0, NULL);
  i2s_set_pin(I2S_PORT, &pinConfig);
  i2s_zero_dma_buffer(I2S_PORT);
}

void loop() {
  if (Serial.available()) {
    char command = Serial.read();

    if (command == 'R' || command == 'r') {
      recordAndSendWav();
    }
  }
}