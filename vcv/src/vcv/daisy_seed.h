// Shim di <daisy_seed.h> per la build VCV Rack.
//
// src/core lo include in due soli punti: driver.h (per daisy::StopwatchTimer)
// e tempo.cpp (per daisy::System::GetNow). Entrambi sono soft-timer in
// millisecondi.
//
// La base dei tempi è il contatore di frame del motore di Rack, non l'orologio
// di sistema: così i timer restano deterministici e sample-accurate anche in
// rendering offline, cosa che il wall-clock del Daisy non garantiva.
#pragma once

#include "daisy.h"

namespace spotykach_vcv
{

// Definite in vcv_time.cpp, l'unica unità di traduzione che include rack.hpp.
// Tenerle fuori dagli header evita di trascinare rack.hpp dentro il firmware.
uint32_t now_us();
uint32_t now_ms();

} // namespace spotykach_vcv

namespace daisy
{

class System
{
  public:
    /// Millisecondi dall'avvio del motore audio.
    static uint32_t GetNow() { return spotykach_vcv::now_ms(); }

    /// Il "tick" qui è già in microsecondi: la conversione è l'identità.
    static uint32_t GetTick() { return spotykach_vcv::now_us(); }

    static uint32_t GetUs() { return spotykach_vcv::now_us(); }

    /// Aritmetica unsigned: l'overflow a ~71 minuti si comporta come sull'hardware.
    static uint32_t GetUsBetweenTicks(uint32_t now, uint32_t then)
    {
        return now - then;
    }
};

/// Equivalente di daisy::StopwatchTimer, sulla base dei tempi del motore.
class StopwatchTimer
{
  public:
    StopwatchTimer()  = default;
    ~StopwatchTimer() = default;

    inline void Init() { Restart(); }

    inline bool HasPassedUs(uint32_t us)
    {
        return (System::GetTick() - last_) >= us;
    }

    inline bool HasPassedMs(uint32_t ms) { return HasPassedUs(ms * 1000); }

    inline void Restart() { last_ = System::GetTick(); }

  private:
    uint32_t last_ = 0;
};

} // namespace daisy
