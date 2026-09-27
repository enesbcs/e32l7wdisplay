#include "LogBuffer.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <unistd.h>

char *LogBuffer::s_buffer = nullptr;
int LogBuffer::s_head = 0;
int LogBuffer::s_count = 0;
SemaphoreHandle_t LogBuffer::s_mutex = nullptr;

static vprintf_like_t s_origVprintf = nullptr;

static int logVprintf(const char *fmt, va_list ap) {
    if (s_origVprintf) {
        s_origVprintf(fmt, ap);
    }
    char buf[384];
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap2);
    va_end(ap2);
    if (len > 0) {
        if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
        std::string line(buf, len);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        LogBuffer::log(line);
    }
    return len;
}

void LogBuffer::init() {
    s_buffer = (char *)heap_caps_malloc(MAX_LINES * LINE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buffer) {
        s_buffer = (char *)malloc(MAX_LINES * LINE_SIZE);
    }
    memset(s_buffer, 0, MAX_LINES * LINE_SIZE);
    s_mutex = xSemaphoreCreateMutex();
    s_origVprintf = esp_log_set_vprintf(logVprintf);
}

void LogBuffer::log(const std::string &line) {
    if (!s_mutex || !s_buffer) return;
    // never block for logging: a wedged holder must not wedge the logger
    // (dropped lines are acceptable loss)
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    strncpy(s_buffer + s_head * LINE_SIZE, line.c_str(), LINE_SIZE - 1);
    s_buffer[s_head * LINE_SIZE + LINE_SIZE - 1] = '\0';
    s_head = (s_head + 1) % MAX_LINES;
    if (s_count < MAX_LINES) s_count++;
    xSemaphoreGive(s_mutex);
}

std::vector<std::string> LogBuffer::getLogs(int maxLines) {
    std::vector<std::string> result;
    if (!s_mutex) return result;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(150)) != pdTRUE) return result;
    int start = s_count < MAX_LINES ? 0 : s_head;
    int cnt = s_count < MAX_LINES ? s_count : MAX_LINES;
    if (cnt > maxLines) {
        start = (start + cnt - maxLines) % MAX_LINES;
        cnt = maxLines;
    }
    for (int i = 0; i < cnt; i++) {
        result.push_back(std::string(s_buffer + ((start + i) % MAX_LINES) * LINE_SIZE));
    }
    xSemaphoreGive(s_mutex);
    return result;
}

void LogBuffer::logf(const char *fmt, ...) {
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) {
        if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
        log(std::string(buf, len));
    }
}

void LogBuffer::logfSerial(const char *fmt, ...) {
    // Ring-bufferbe is, de az igazi serial-monitorra IS (s_origVprintf-en át).
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) {
        if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
        printf("%s\n", buf);
        log(std::string(buf, len));
    }
}

void LogBuffer::clear() {
    if (!s_mutex || !s_buffer) return;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    s_head = 0;
    s_count = 0;
    memset(s_buffer, 0, MAX_LINES * LINE_SIZE);
    xSemaphoreGive(s_mutex);
}

std::string LogBuffer::dump(int maxLines) {
    auto lines = getLogs(maxLines);
    std::string out;
    for (auto &l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}
