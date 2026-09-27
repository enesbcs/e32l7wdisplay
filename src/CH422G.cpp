#include "CH422G.hpp"
#include "PinConfig.hpp"
#include "LogBuffer.hpp"
#include "esp_io_expander.hpp"


esp_expander::CH422G *CH422G::s_expander = nullptr;
uint8_t CH422G::s_output = 0xFF;
bool CH422G::s_initialized = false;
bool CH422G::s_expanderExternal = false;

bool CH422G::begin() {
    if (s_initialized) return true;

    // If an external expander was set via setExpander(), skip I2C init
    if (!s_expanderExternal) {
        s_expander = new esp_expander::CH422G(
            I2C_PINS::SCL,
            I2C_PINS::SDA,
            ESP_IO_EXPANDER_I2C_CH422G_ADDRESS
        );

        if (!s_expander->init()) {
            LogBuffer::logf("I2C init failed");
            delete s_expander;
            s_expander = nullptr;
            return false;
        }

        if (!s_expander->begin()) {
            LogBuffer::logf("CH422G begin failed");
            delete s_expander;
            s_expander = nullptr;
            return false;
        }

        s_expander->enableOC_PushPull();
        s_expander->enableAllIO_Output();

        for (int i = 0; i < 8; i++) {
            s_expander->digitalWrite(i, 1);
        }
    }

    s_output = 0xFF;
    s_initialized = true;
    LogBuffer::logf("CH422G initialized (Espressif lib), output=0xFF");
    return true;
}

void CH422G::setExpander(esp_expander::CH422G *expander) {
    s_expander = expander;
    s_expanderExternal = true;
    s_initialized = false; // begin() will set this
}

bool CH422G::write(uint8_t value) {
    if (!s_initialized || !s_expander) return false;

    uint8_t changed = s_output ^ value;
    for (int i = 0; i < 8; i++) {
        if (changed & (1 << i)) {
            s_expander->digitalWrite(i, (value >> i) & 1);
        }
    }

    s_output = value;
    return true;
}

uint8_t CH422G::read() {
    return s_output;
}

bool CH422G::setPin(uint8_t pin_mask, bool state) {
    uint8_t value = s_output;
    if (state) {
        value |= pin_mask;
    } else {
        value &= ~pin_mask;
    }
    return write(value);
}

bool CH422G::setBacklight(bool on) {
    return setPin(EXIO3, on);
}

bool CH422G::setSD_CS(bool active) {
    return setPin(EXIO5, !active);
}
