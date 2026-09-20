#include <Arduino.h>
#include <driver/i2s.h>
#include <stdlib.h>
#include <string.h>

// INMP441 wiring
#define I2S_WS   42
#define I2S_SD   41
#define I2S_SCK  40
#define I2S_PORT I2S_NUM_0

#define SAMPLE_RATE     16000
#define MIC_GAIN        8  // 修改這個數字後，必須重新燒錄 ESP32 才會生效。
#define MIN_SECONDS     1
#define MAX_SECONDS     10

char commandBuffer[32];
size_t commandLength = 0;

void writeWavHeader(uint32_t dataSize) {
  const uint32_t fileSize = 36 + dataSize;
  const uint32_t fmtSize = 16;
  const uint16_t audioFormat = 1;
  const uint16_t channels = 1;
  const uint32_t sampleRate = SAMPLE_RATE;
  const uint16_t bitsPerSample = 16;
  const uint16_t blockAlign = channels * bitsPerSample / 8;
  const uint32_t byteRate = sampleRate * blockAlign;
  Serial.write("RIFF", 4);
  Serial.write((const uint8_t *)&fileSize, 4);
  Serial.write("WAVE", 4);
  Serial.write("fmt ", 4);
  Serial.write((const uint8_t *)&fmtSize, 4);
  Serial.write((const uint8_t *)&audioFormat, 2);
  Serial.write((const uint8_t *)&channels, 2);
  Serial.write((const uint8_t *)&sampleRate, 4);
  Serial.write((const uint8_t *)&byteRate, 4);
  Serial.write((const uint8_t *)&blockAlign, 2);
  Serial.write((const uint8_t *)&bitsPerSample, 2);
  Serial.write("data", 4);
  Serial.write((const uint8_t *)&dataSize, 4);
}

void recordAndSendWav(uint16_t seconds) {
  const uint32_t totalSamples = SAMPLE_RATE * seconds;
  const uint32_t pcmBytes = totalSamples * sizeof(int16_t);
  const uint32_t wavBytes = 44 + pcmBytes;
  Serial.printf("WAV_BEGIN %lu\n", (unsigned long)wavBytes);
  delay(30);
  writeWavHeader(pcmBytes);

  for (uint32_t i = 0; i < totalSamples; i++) {
    int32_t rawSample = 0;
    size_t bytesRead = 0;
    int16_t pcmSample = 0;
    i2s_read(I2S_PORT, &rawSample, sizeof(rawSample), &bytesRead, portMAX_DELAY);
    if (bytesRead == sizeof(rawSample)) {
      int32_t amplified = (rawSample >> 16) * MIC_GAIN;
      if (amplified > 32767) amplified = 32767;
      else if (amplified < -32768) amplified = -32768;
      pcmSample = (int16_t)amplified;
    }
    Serial.write((const uint8_t *)&pcmSample, sizeof(pcmSample));
  }
  Serial.flush();
  Serial.println("WAV_END");
}

void handleCommand(const char *command) {
  if (strcmp(command, "R") == 0 || strcmp(command, "r") == 0) {
    recordAndSendWav(1);
    return;
  }
  if (strncmp(command, "REC ", 4) != 0) {
    Serial.println("ERROR UNKNOWN_COMMAND");
    return;
  }
  char *end = nullptr;
  const long seconds = strtol(command + 4, &end, 10);
  if (*end != '\0' || seconds < MIN_SECONDS || seconds > MAX_SECONDS) {
    Serial.printf("ERROR DURATION_%d_TO_%d\n", MIN_SECONDS, MAX_SECONDS);
    return;
  }
  recordAndSendWav((uint16_t)seconds);
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
  while (Serial.available()) {
    const char received = (char)Serial.read();
    if (received == '\r') continue;
    if (received == '\n') {
      commandBuffer[commandLength] = '\0';
      if (commandLength > 0) handleCommand(commandBuffer);
      commandLength = 0;
      continue;
    }
    if (commandLength < sizeof(commandBuffer) - 1) commandBuffer[commandLength++] = received;
    else {
      commandLength = 0;
      Serial.println("ERROR COMMAND_TOO_LONG");
    }
  }
}
