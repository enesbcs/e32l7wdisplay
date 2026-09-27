#pragma once

#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class LogBuffer {
public:
    static void init();
    static void log(const std::string &line);
    static void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    static void logfSerial(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    static std::vector<std::string> getLogs(int maxLines = 40);
    static std::string dump(int maxLines = 40);
    static void clear();

private:
    static constexpr int MAX_LINES = 40;
    static constexpr int LINE_SIZE = 384;
    static char *s_buffer;
    static int s_head;
    static int s_count;
    static SemaphoreHandle_t s_mutex;
};
