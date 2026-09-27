#pragma once

// Passive BLE advertising scanner (Bluedroid).
// Decodes: BTHome v2 (plaintext), Xiaomi MiBeacon, ATC1441 custom firmware.
// Sensor ids are the advertiser MAC address (e.g. "a4c138abcdef12").
class BLEScan {
public:
    static void begin();
    static void end();
};