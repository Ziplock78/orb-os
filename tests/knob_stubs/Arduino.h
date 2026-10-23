#pragma once
#include <stdint.h>
#define IRAM_ATTR
#define HIGH 1
#define LOW 0
#define INPUT_PULLUP 2
#define CHANGE 3
uint32_t millis();
inline void pinMode(int, int) {}
inline int digitalRead(int) { return HIGH; }
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterrupt(int, void (*)(), int) {}
struct TestSerial {
    void println(const char *) {}
    template<typename... Args> void printf(const char *, Args...) {}
};
inline TestSerial Serial;
