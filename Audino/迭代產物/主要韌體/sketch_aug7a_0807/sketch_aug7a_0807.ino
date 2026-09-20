#include <driver/i2s.h>

// ====== INMP441 接腳 ======
#define I2S_WS 42 // LRCLK
#define I2S_SD 41 // DOUT
#define I2S_SCK 40 // BCLK

#define I2S_PORT I2S_NUM_0

void setup() {
Serial.begin(115200);

// I2S 設定
const i2s_config_t i2s_config = {
.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
.sample_rate = 16000,
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

Serial.println("INMP441 Ready!");
}

/*void loop() {

  int32_t sample = 0;
  size_t bytes_read;

  long sum = 0;

  // 取 100 次採樣
  for (int i = 0; i < 100; i++) {

    i2s_read(
      I2S_PORT,
      &sample,
      sizeof(sample),
      &bytes_read,
      portMAX_DELAY
    );

    // INMP441 24bit資料
    sample = sample >> 14;

    sum += abs(sample);
  }

  // 平均音量
  int volume = sum / 100;

  Serial.println(volume);
  
}*/
void loop() {

  int32_t sample = 0;
  size_t bytes_read;

  long sum = 0;

  // 讀取100次聲音
  for(int i = 0; i < 100; i++) {

    i2s_read(
      I2S_PORT,
      &sample,
      sizeof(sample),
      &bytes_read,
      portMAX_DELAY
    );

    sample = sample >> 14;

    sum += abs(sample);
  }


  // 平均音量
  int volume = sum / 100;


  // 顯示音量條
  int level = volume / 500;

  for(int i = 0; i < level; i++){
    Serial.print("#");
  }

  Serial.println();
}