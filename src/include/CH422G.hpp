#pragma once

#include <cstdint>

namespace esp_expander { class CH422G; }

class CH422G {
public:
    static constexpr uint8_t EXIO1 = (1 << 0);
    static constexpr uint8_t EXIO2 = (1 << 1);
    static constexpr uint8_t EXIO3 = (1 << 2);
    static constexpr uint8_t EXIO4 = (1 << 3);
    static constexpr uint8_t EXIO5 = (1 << 4);
    static constexpr uint8_t EXIO6 = (1 << 5);
    static constexpr uint8_t EXIO7 = (1 << 6);
    static constexpr uint8_t EXIO8 = (1 << 7);

    static bool begin();
    static bool write(uint8_t value);
    static uint8_t read();
    static bool setPin(uint8_t pin_mask, bool state);
    static bool setBacklight(bool on);
    static bool setSD_CS(bool active);

    static void setExpander(esp_expander::CH422G *expander);

private:
    static esp_expander::CH422G *s_expander;
    static uint8_t s_output;
    static bool s_initialized;
    static bool s_expanderExternal;
};
