#include <Arduino.h>
#include <driver/i2s.h>

// 定義 I2S 腳位
#define I2S_WS   42
#define I2S_SD   41
#define I2S_SCK  40
#define I2S_PORT I2S_NUM_0


#define bufferLen 64
int32_t sBuffer[bufferLen];

// 音量計算相關變數
int16_t min_sample = 32767;
int16_t max_sample = -32768;
int sample_count = 0;
const int SAMPLES_PER_UPDATE = 512; // 每 512 個 sample 更新一次音量條

void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = bufferLen,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_start(I2S_PORT);
}

void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  setupI2S();
  Serial.println("I2S 麥克風音量條測試開始...");
}

void loop() {
  size_t bytes_read = 0;
  esp_err_t result = i2s_read(I2S_PORT, &sBuffer, sizeof(sBuffer), &bytes_read, portMAX_DELAY);
  
  if (result == ESP_OK && bytes_read > 0) {
    int samples_read = bytes_read / sizeof(int32_t);
    
    for (int i = 0; i < samples_read; i++) {
      int16_t sample = sBuffer[i] >> 14; 
      
      // 記錄此梯次的最大與最小值
      if (sample < min_sample) min_sample = sample;
      if (sample > max_sample) max_sample = sample;
      
      sample_count++;

      // 當累積足夠的採樣點時，計算峰值差並顯示音量條
      if (sample_count >= SAMPLES_PER_UPDATE) {
        int peak_to_peak = max_sample - min_sample; // 計算振幅
        
        // 將振幅 (約 0 ~ 12000) 映射為 0 ~ 30 個 '0'
        int bar_length = map(peak_to_peak, 200, 12000, 0, 30);
        bar_length = constrain(bar_length, 0, 30); // 限制長度範圍

        // 印出音量文字條
        Serial.print("[");
        for (int b = 0; b < bar_length; b++) {
          Serial.print("0");
        }
        for (int b = bar_length; b < 30; b++) {
          Serial.print(" ");
        }
        Serial.print("] P2P: ");
        Serial.println(peak_to_peak);

        // 重置區域變數
        min_sample = 32767;
        max_sample = -32768;
        sample_count = 0;
      }
    }
  }
}