// Rimpiazzo desktop di src/hw/buffer.sdram.h.
//
// Stessa API dispenser di SDRAMBuffer, ma su heap e — differenza essenziale —
// **per istanza**, non singleton: due moduli Spotykach nella stessa patch
// devono avere buffer separati.
#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "core/buffer.h"
#include "core/config.h"
#include "core/detector.h"
#include "core/event.h"
#include "core/fx.h"
#include "core/track.h"

namespace spotykach
{

/// Il motore gira sempre a questo sample rate: le costanti DSP del firmware
/// sono tarate su 48 kHz e non vengono toccate.
static constexpr float kEngineSampleRate = 48000.f;

/// Dimensione del blocco, identica al firmware: 96 campioni = 2 ms, che è
/// esattamente l'intervallo dichiarato a SynClock::Init in driver.cpp.
static constexpr size_t kEngineBlockSize = 96;

class BufferPool
{
  public:
    /// Lunghezza massima del loop per deck, in secondi. 42 s è il valore
    /// dell'hardware; valori più bassi riducono i ~36 MB per istanza.
    explicit BufferPool(float source_seconds = 42.f) { allocate(source_seconds); }

    ~BufferPool() = default;

    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    void allocate(float source_seconds)
    {
        _source_frames
            = static_cast<size_t>(source_seconds * kEngineSampleRate);

        // Solo due source buffer: il terzo (undo) di buffer.sdram.cpp non è
        // usato da Core::init e ci costerebbe altri 16 MB.
        for (auto& b : _source)
            b.assign(_source_frames, Buffer::Frame{0.f, 0.f});

        for (auto& b : _delay)
            b.assign(Fx::kEchoDelayBufferLength, 0.f);

        for (auto& b : _detector)
            b.assign(Detector::kWindow, 0.f);

        _slices_a.assign(kMaxSlicePointCount, 0);
        _slices_b.assign(kMaxSlicePointCount, 0);
        _track_a.assign(Track::kLength, Event{});
        _track_b.assign(Track::kLength, Event{});

        // Detector::init conserva il `float**` che riceve, non i due puntatori:
        // le righe devono quindi vivere quanto il pool. Sull'hardware erano
        // `static` dentro Core::init, il che le rendeva eterne — ma anche
        // condivise fra istanze, che su desktop non va bene.
        for (size_t d = 0; d < kDecks; d++) {
            for (size_t c = 0; c < kChannels; c++) {
                _detector_rows[d][c] = _detector[d * kChannels + c].data();
                _delay_rows[d][c]    = _delay[d * kChannels + c].data();
            }
        }

        reset_dispensers();
    }

    /// Righe stabili da passare a Deck::Params::detect_buf / delay_buf.
    float** detectorRow(size_t deck) { return _detector_rows[deck]; }
    float** delayRow(size_t deck) { return _delay_rows[deck]; }

    /// Da chiamare prima di ogni Core::init, così una re-inizializzazione
    /// riparte dal primo buffer invece di esaurire il pool.
    void reset_dispensers()
    {
        _source_given   = 0;
        _delay_given    = 0;
        _detector_given = 0;
    }

    Buffer::Frame* sourceBuffer()
    {
        return _source[_source_given++ % _source.size()].data();
    }

    size_t sourceBufferSize() const { return _source_frames; }

    float* delayBuffer()
    {
        return _delay[_delay_given++ % _delay.size()].data();
    }

    float* detectorBuffer()
    {
        return _detector[_detector_given++ % _detector.size()].data();
    }

    size_t* slices_a() { return _slices_a.data(); }
    size_t* slices_b() { return _slices_b.data(); }

    Event* track_buffer_a() { return _track_a.data(); }
    Event* track_buffer_b() { return _track_b.data(); }

    /// Byte allocati, per il menu contestuale.
    size_t bytes() const
    {
        return _source.size() * _source_frames * sizeof(Buffer::Frame)
               + _delay.size() * Fx::kEchoDelayBufferLength * sizeof(float)
               + _detector.size() * Detector::kWindow * sizeof(float);
    }

  private:
    static constexpr size_t kDecks    = 2;
    static constexpr size_t kChannels = 2;

    std::array<std::vector<Buffer::Frame>, kDecks>            _source;
    std::array<std::vector<float>, kDecks * kChannels>        _delay;
    std::array<std::vector<float>, kDecks * kChannels>        _detector;
    std::vector<size_t>        _slices_a;
    std::vector<size_t>        _slices_b;
    std::vector<Event>         _track_a;
    std::vector<Event>         _track_b;

    float* _detector_rows[kDecks][kChannels] = {};
    float* _delay_rows[kDecks][kChannels]    = {};

    size_t _source_frames   = 0;
    size_t _source_given    = 0;
    size_t _delay_given     = 0;
    size_t _detector_given  = 0;
};

} // namespace spotykach
