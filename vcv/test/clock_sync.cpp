// Che cosa si aspetta il `Driver` sull'ingresso di clock.
//
// Alimenta il motore con un clock esterno a BPM noto, variando quanti impulsi
// per quarto manda il mittente, e misura due cose: il tempo che il driver
// dichiara, e la cadenza reale dei quarti — che è quella su cui batte il click.
//
//   make -f test/Makefile clock
//
// Il modulo chiama `Driver::tick` una volta per blocco, quindi i fronti di
// salita li campioniamo alla stessa risoluzione (2 ms), come in Rack.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "buffer_pool.h"
#include "core/core.h"
#include "core/mode.h"

using namespace spotykach;

static uint64_t g_frames = 0;

namespace spotykach_vcv
{
uint32_t now_us() { return (uint32_t)((g_frames * 1000000ULL) / 48000ULL); }
uint32_t now_ms() { return (uint32_t)((g_frames * 1000ULL) / 48000ULL); }
} // namespace spotykach_vcv

static uint64_t g_first_quarter = 0;
static uint64_t g_last_quarter = 0;
static int g_quarters = 0;
static bool g_counting = false;

/// Un giro di prova. `sender_ppqn` è quanti impulsi per quarto manda il modulo
/// di clock; `driver_ppqn` quanti ne dichiara il nostro parametro IN PPQN.
static double run(const float bpm, const int sender_ppqn, const int driver_ppqn,
                  const float seconds)
{
    BufferPool pool(2.f); // il buffer qui non serve: teniamolo piccolo
    Core core;
    g_quarters = 0;
    g_counting = false;
    core.driver().set_on_quarter([](const bool) {
        if (!g_counting) return;
        if (g_quarters++ == 0) g_first_quarter = g_frames;
        g_last_quarter = g_frames;
    });
    core.driver().set_on_clock_out([]() {});
    core.init(kEngineSampleRate, (float)kEngineBlockSize, pool);
    core.set_mix(.5f);
    core.set_click_mix(0.f);

    // internal -> ts4, come fa updateGlobalParams; poi il PPQN dichiarato,
    // che toggle_source ha appena riportato a 4.
    core.driver().toggle_source();
    core.driver().set_external_ppqn((uint32_t)driver_ppqn);

    const int block = (int)kEngineBlockSize;
    const int blocks = (int)(seconds * kEngineSampleRate / block);
    const double pulse_period = 60.0 / (bpm * sender_ppqn); // secondi fra due impulsi

    float in_l[kEngineBlockSize] = {};
    float in_r[kEngineBlockSize] = {};
    float out_l[kEngineBlockSize];
    float out_r[kEngineBlockSize];
    float* in_ptr[2] = {in_l, in_r};
    float* out_ptr[2] = {out_l, out_r};

    // La prima metà del giro serve al clock per agganciarsi: contiamo i quarti
    // solo dopo, e fra il primo e l'ultimo, così la misura non dipende da dove
    // cadono i bordi della finestra.
    const int settle = blocks / 2;
    double next_pulse = 0.0;

    g_frames = 0;
    for (int b = 0; b < blocks; b++) {
        const double t = (double)(b * block) / kEngineSampleRate;

        bool rising = false;
        if (t >= next_pulse) {
            rising = true;
            next_pulse += pulse_period;
        }

        if (b == settle) g_counting = true;

        core.driver().tick(rising);
        core.prepare();
        core.process(in_ptr, out_ptr, block);
        g_frames += block;
    }

    double measured_bpm = 0.0;
    if (g_quarters >= 2) {
        const double span = (double)(g_last_quarter - g_first_quarter) / kEngineSampleRate;
        measured_bpm = (g_quarters - 1) * 60.0 / span;
    }

    std::printf("  mittente %2d PPQN, IN PPQN %2d  ->  driver %6.1f BPM,"
                "  quarti %6.1f BPM  (x%.2f)\n",
                sender_ppqn, driver_ppqn, core.driver().tempo(), measured_bpm,
                measured_bpm / bpm);
    return measured_bpm / bpm;
}

int main(int argc, char** argv)
{
    float seconds = 24.f;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a.rfind("--seconds=", 0) == 0) seconds = std::stof(a.substr(10));
    }

    const float bpm = 120.f;
    std::printf("Sincronizzazione al clock esterno, mittente a %.0f BPM (%.0f s per giro)\n",
                bpm, seconds);

    std::printf("\nIN PPQN fisso a 4, come sull'hardware — è il difetto da cui siamo partiti:\n");
    for (int ppqn : {1, 2, 4, 8, 24}) run(bpm, ppqn, 4, seconds);

    std::printf("\nIN PPQN pari a quello del mittente:\n");
    bool ok = true;
    for (int ppqn : {1, 2, 3, 4, 6, 8, 12, 16, 24, 48}) {
        const double ratio = run(bpm, ppqn, ppqn, seconds);
        if (std::fabs(ratio - 1.0) > 0.02) ok = false;
    }

    std::printf("\n%s\n", ok ? "OK: con il PPQN giusto il click batte il tempo del mittente."
                             : "ERRORE: qualche combinazione non batte il tempo del mittente.");
    return ok ? 0 : 1;
}
