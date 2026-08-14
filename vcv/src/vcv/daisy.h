// Shim di <daisy.h> per la build VCV Rack.
//
// Il firmware include <daisy.h> da common.h solo per il logger e un paio di
// macro di utilità. Qui forniamo la superficie minima, senza alcun hardware.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>

// DSY_CLAMP / DSY_MIN / DSY_MAX li definisce DaisySP in Utility/dsp.h, che
// common.h include subito dopo questo header: non vanno duplicati qui.

// Formattazione float senza printf floating point: usata da expose.h, che è
// incluso (ma mai chiamato) da diversi file di src/core.
#ifndef FLT_FMT
#define FLT_FMT(n) "%d.%0" #n "d"
#define FLT_VAR(n, x)                                        \
    static_cast<int>(x),                                     \
        std::abs(static_cast<int>(((x) - static_cast<int>(x)) \
                                  * std::pow(10, n)))
#endif

#ifndef FLT_FMT3
#define FLT_FMT3 FLT_FMT(3)
#define FLT_VAR3(x) FLT_VAR(3, x)
#endif

namespace daisy
{

enum LoggerDestination
{
    LOGGER_NONE,
    LOGGER_INTERNAL,
    LOGGER_EXTERNAL,
    LOGGER_SEMIHOST,
};

// Logger no-op. Le firme ricalcano quelle di libDaisy così che i (pochi) siti
// di chiamata rimasti in src/core compilino senza modifiche.
template <LoggerDestination dest = LOGGER_NONE>
class Logger
{
  public:
    static void StartLog(bool wait_for_pc = false) { (void)wait_for_pc; }

    template <typename... Va>
    static void Print(const char* format, Va... va)
    {
        (void)format;
    }

    template <typename... Va>
    static void PrintLine(const char* format, Va... va)
    {
        (void)format;
    }

    static void PrintLineV(const char* format, va_list va)
    {
        (void)format;
        (void)va;
    }
};

} // namespace daisy
