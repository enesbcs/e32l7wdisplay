#pragma once

#include "driver/gpio.h"

// NOTE: only the actually-used pins live here. LCD panel pinout, timings
// and the EXIO bit table are owned by esp_panel_board_custom_conf.h (root)
// and CH422G (EXIO1..8, it drives the expander); J13 mapping: EXIO3 = LCD
// backlight EN, EXIO4 = LCD reset, EXIO5 = SD CS. No SD card code exists.

// I2C for CH422G GPIO expander
namespace I2C_PINS {
    constexpr gpio_num_t SDA = GPIO_NUM_8;
    constexpr gpio_num_t SCL = GPIO_NUM_9;
}

// Display parameters
namespace DISPLAY {
    constexpr int H_RES = 800;
    constexpr int V_RES = 480;
}
