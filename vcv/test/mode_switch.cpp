// Regressione dei due difetti usciti dal collaudo in MetaRack del 2026-08-17:
//
//   1) cambiare modo Reel -> Slice faceva crashare (con buffer vuoto, sempre);
//   2) a volte il click non arrivava all'uscita audio.
//
// Il secondo caso gira quattro volte con la memoria del motore sporcata da un
// byte diverso: il firmware presume di vivere in `.bss` azzerata, e i membri
// senza default facevano dipendere il clock da ciò che c'era in RAM. Le quattro
// righe devono venire identiche.
//
//   make -f test/Makefile modes

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "buffer_pool.h"
#include "core/core.h"
#include "core/event.h"
#include "core/mode.h"

using namespace spotykach;

static uint64_t g_frames = 0;

namespace spotykach_vcv
{
uint32_t now_us() { return (uint32_t)((g_frames * 1000000ULL) / 48000ULL); }
uint32_t now_ms() { return (uint32_t)((g_frames * 1000ULL) / 48000ULL); }
} // namespace spotykach_vcv

static const int kBlock = (int)kEngineBlockSize;
static int g_failures = 0;

static void check(const bool ok, const char* what)
{
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FALLITO");
    if (!ok) g_failures++;
}

// Quello che il modulo fa a ogni blocco, ridotto all'essenziale.
struct Rig {
    BufferPool pool{4.f};
    Core* core = nullptr;
    std::vector<unsigned char> storage;

    float inL[kEngineBlockSize];
    float inR[kEngineBlockSize];
    float outL[kEngineBlockSize];
    float outR[kEngineBlockSize];

    int quarters = 0;
    int clock_outs = 0;
    double peak = 0.0;
    int non_finite = 0;
    unsigned rng = 22222;

    /// `fill` è il byte con cui si sporca la memoria prima di costruire Core.
    /// Il modulo la azzera (ZeroedEngine in DoubleDeck.cpp, come la `.bss`
    /// dell'hardware): qui la sporchiamo di proposito perché il motore non
    /// debba più dipenderne.
    explicit Rig(int fill = -1) {
        storage.resize(sizeof(Core) + 64);
        if (fill >= 0) std::memset(storage.data(), fill, storage.size());
        core = new (storage.data()) Core();

        core->driver().set_on_quarter([this](const bool) { quarters++; });
        core->driver().set_on_clock_out([this] { clock_outs++; });

        core->init(kEngineSampleRate, (float)kEngineBlockSize, pool);
        core->set_route(Route::Stereo);
        // Core non dà un default né al crossfade né al click: nel modulo li
        // scrive il primo giro di parametri.
        core->set_mix(.5f);
        core->set_click_mix(0.f);
        for (auto ref : {Deck::A, Deck::B}) core->deck(ref).set_mode(Mode::Reel);
    }

    ~Rig() { core->~Core(); }

    void block(bool noise) {
        for (int i = 0; i < kBlock; i++) {
            rng = rng * 1664525u + 1013904223u;
            const float n = noise ? ((int)(rng >> 8) / 8388608.f - 1.f) * .3f : 0.f;
            inL[i] = n;
            inR[i] = n * .8f;
        }

        for (auto ref : {Deck::A, Deck::B}) {
            auto& d = core->deck(ref);
            d.voxs().pitch_speed_mod_in(1.f);
            d.inout_mix_mod_in(0.f);
            d.voxs().set_start_mod(0.f);
            d.voxs().set_size_mod(0.f);
        }
        core->mix_mod_in(0.f);

        core->driver().tick(false);
        core->prepare();

        float* in[2] = {inL, inR};
        float* out[2] = {outL, outR};
        core->process(in, out, kBlock);

        for (int i = 0; i < kBlock; i++) {
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) non_finite++;
            peak = std::fmax(peak, std::fmax(std::fabs(outL[i]), std::fabs(outR[i])));
        }
        for (auto ref : {Deck::A, Deck::B}) core->deck(ref).voxs().read_reset_is_triggered();
        g_frames += kBlock;
    }

    void run(float seconds, bool noise = true) {
        const int n = (int)(seconds * kEngineSampleRate / kBlock);
        for (int i = 0; i < n; i++) block(noise);
    }
};

// --- 1) cambio di modo -------------------------------------------------------
//
// Con un buffer più corto di un passo di griglia, `Deck::make_grid` passava a
// `Generator::auto_cue` un conteggio di slice nullo: `slice_count - 1` su size_t
// diventava SIZE_MAX e apply_dimensions finiva per fare un modulo per zero.
// Su Cortex-M7 UDIV per zero dà 0 in silenzio, su x86 è una trap.

static void case_mode(const char* label, float record_seconds)
{
    std::printf("--- Reel -> Slice, %s ---\n", label);
    Rig rig;
    rig.run(.2f);

    if (record_seconds > 0.f) {
        rig.core->set_source(Deck::Source::external, Deck::A);
        rig.core->deck(Deck::A).toggle_recording();
        rig.run(record_seconds);
        rig.core->deck(Deck::A).toggle_recording();
        rig.run(.1f);
    }

    auto& deck = rig.core->deck(Deck::A);
    std::printf("  buffer deck A: %.3f s (rec_size %d)\n",
                deck.buffer().rec_size() / kEngineSampleRate, (int)deck.buffer().rec_size());
    std::fflush(stdout);

    deck.set_mode(Mode::Slice);
    rig.core->infer_panner_mode();
    rig.run(.5f); // qui gira apply_dimensions: prima moriva sul primo blocco

    check(deck.mode() == Mode::Slice, "il deck e' in Slice");
    check(rig.non_finite == 0, "nessun campione non finito in uscita");
    std::printf("\n");
}

// --- 2) il click -------------------------------------------------------------
//
// Due difetti nella stessa riga di segnalazione. `SynClock::_external_clock`
// senza default: se cadeva vero, Driver::tick fermava il clock interno dopo un
// secondo e con lui click, loop in Slice e modulatori sincronizzati — per
// sempre. `Driver::_quarter_tick_count` senza default: il primo quarto
// arrivava fino a 127 sedicesimi in ritardo, cioè 16 s a 120 bpm.

static void case_click()
{
    std::printf("--- click: 20 s di clock interno a 120 bpm ---\n");
    // Deck vuoti e fermi: in uscita non c'è altro che il click.
    const int kQuarters = 40;   // 20 s a 120 bpm
    const int kClockOuts = 960; // 24 PPQN
    for (int fill : {0x00, 0x7f, 0xff, 0x5a}) {
        Rig rig(fill);
        rig.core->set_click_mix(1.f);
        rig.run(20.f, /*noise*/ false);
        std::printf("  memoria a 0x%02x -> quarti %3d   clock out %4d   picco %.4f\n",
                    fill, rig.quarters, rig.clock_outs, rig.peak);
        char what[80];
        std::snprintf(what, sizeof(what), "0x%02x: %d quarti, %d clock out, click udibile",
                      fill, kQuarters, kClockOuts);
        check(rig.quarters == kQuarters && rig.clock_outs == kClockOuts && rig.peak > 1e-3,
              what);
    }
    std::printf("\n");
}

int main(int argc, char** argv)
{
    std::string only;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a.rfind("--case=", 0) == 0) only = a.substr(7);
    }

    if (only.empty() || only == "mode") {
        case_mode("buffer vuoto", 0.f);
        case_mode("0.05 s registrati", .05f);
        case_mode("2 s registrati", 2.f);
    }
    if (only.empty() || only == "click") case_click();

    std::printf("%s (%d fallimenti)\n", g_failures == 0 ? "TUTTO OK" : "REGRESSIONE", g_failures);
    return g_failures == 0 ? 0 : 1;
}
