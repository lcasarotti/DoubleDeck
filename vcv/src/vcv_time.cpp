// Base dei tempi per lo shim daisy_seed.h.
//
// Unica unità di traduzione in cui rack.hpp e gli header del firmware possono
// convivere: qui non si include nulla di src/core.
#include <rack.hpp>

namespace spotykach_vcv
{

uint32_t now_us()
{
    const double sr = APP->engine->getSampleRate();
    if (sr <= 0.f)
        return 0;
    const int64_t frame = APP->engine->getFrame();
    return (uint32_t)((frame * 1000000LL) / (int64_t)sr);
}

uint32_t now_ms()
{
    const double sr = APP->engine->getSampleRate();
    if (sr <= 0.f)
        return 0;
    const int64_t frame = APP->engine->getFrame();
    return (uint32_t)((frame * 1000LL) / (int64_t)sr);
}

} // namespace spotykach_vcv
