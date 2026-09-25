# YD ESP32-S3 N16R8 — TFLite + OLED status firmware

This folder is a new, independent sketch. It does not replace or edit the
working diagnostic sketch.

## Wiring

| Module signal | YD ESP32-S3 N16R8 |
| --- | --- |
| INMP441 VDD | 3V3 |
| INMP441 GND | GND |
| INMP441 SCK | GPIO4 |
| INMP441 WS | GPIO5 |
| INMP441 SD | GPIO6 |
| INMP441 L/R | GND |
| OLED GND | GND |
| OLED VDD | 3V3 |
| OLED SCK / SCL | GPIO9 |
| OLED SDA | GPIO8 |

The OLED header in the supplied photograph is ordered, from left to right:
GND, VDD, SCK (I2C SCL), SDA.

GPIO7 is deliberately reserved for a future I2S speaker amplifier. Do not
connect an amplifier in this first OLED-only test.

## Arduino IDE

1. Select ESP32S3 Dev Module.
2. Set Flash Size to 16MB.
3. For this YD N16R8 board, set Flash Mode to DIO 80MHz.
4. Set PSRAM to OPI PSRAM.
5. Set Partition Scheme to 16M Flash (3MB APP/9.9MB FATFS). This leaves room
   for future replacement sound files.
6. The required U8g2 Arduino library is installed in the Arduino libraries
   folder. Restart Arduino IDE once if it was already open.
7. Compile and upload this sketch.

At boot, Serial Monitor should report either the OLED address (0x3C or
0x3D) or state that no I2C OLED was found. After roughly one second, the
screen should display the current raw classification and confidence.

## If I2C is detected but the screen remains black

This board layout is normally SH1106. If the Serial Monitor reports an OLED
address but the panel is blank, change the single display constructor near the
top of the .ino from:

~~~cpp
U8G2_SH1106_128X64_NONAME_F_HW_I2C
~~~

to:

~~~cpp
U8G2_SSD1306_128X64_NONAME_F_HW_I2C
~~~

No wiring change is needed.

## Important

The model input conversion remains exactly the verified TFLite path:
(raw >> 16) * 4, clipped to signed 16-bit. OLED rendering happens outside
the real-time I2S capture task, so it does not alter microphone capture or
classifier input timing.
