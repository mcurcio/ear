// no_raw_sleeps_bad.cpp's later-header case: a header no_raw_sleeps.hpp does not read first, which names a raw sleep in
// its own inline code, as a library's header might.
#pragma once
#include <chrono>
#include <thread>
inline void napALittle() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
