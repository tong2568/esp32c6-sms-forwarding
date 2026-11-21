#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

// ----------- LED -----------
// ESP32-C6 SuperMini 板载用户状态灯接在 GPIO15（普通 GPIO，可直接 digitalWrite）。
// 注：该板另有一颗 WS2812 可寻址 RGB 灯接在 GPIO8，本项目未使用。
static const uint8_t LED_BUILTIN = 15;
#define BUILTIN_LED LED_BUILTIN
#define LED_BUILTIN LED_BUILTIN

// ----------- BOOT -----------
// 板载 BOOT 按键接在 GPIO9。
static const uint8_t BOOT_BUILTIN = 9;
#define BOOT_BUILTIN BOOT_BUILTIN

#define TX1 0
#define RX1 1

// ----------- UART -----------
static const uint8_t TX = 16;
static const uint8_t RX = 17;

// ----------- I2C -----------
static const uint8_t SDA = 20;
static const uint8_t SCL = 19;

// ----------- SPI -----------
static const uint8_t SS   = 4;
static const uint8_t MOSI = 5;
static const uint8_t MISO = 6;
static const uint8_t SCK  = 7;

// ----------- ADC -----------
static const uint8_t A0 = 0;
static const uint8_t A1 = 1;
static const uint8_t A2 = 2;
static const uint8_t A3 = 3;
static const uint8_t A4 = 4;
static const uint8_t A5 = 5;
static const uint8_t A6 = 6;

#endif /* Pins_Arduino_h */
