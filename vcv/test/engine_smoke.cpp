// Harness di collaudo del motore, senza Rack.
//
// Istanzia BufferPool + Core esattamente come fa il modulo e ne esegue N
// secondi di blocchi, così i crash nel percorso audio si vedono in un secondo
// invece che aprendo Rack.
//
// Compilare ed eseguire:
//   make -f test/Makefile run
//
// Con --no-callbacks riproduce di proposito il crash da std::function vuota,
// per verificare che la diagnosi sia quella giusta.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "buffer_pool.h"
#include "core/core.h"
#include "core/event.h"
#include "core/mode.h"

using namespace spotykach;

// Base dei tempi: nel plugin viene da APP->engine->getFrame(). Qui la
// pilotiamo noi, così il test è deterministico.
static uint64_t g_frames = 0;

namespace spotykach_vcv
{
uint32_t now_us()
{
    return (uint32_t)((g_frames * 1000000ULL) / 48000ULL);
}
uint32_t now_ms()
{
    return (uint32_t)((g_frames * 1000ULL) / 48000ULL);
}
} // namespace spotykach_vcv

static int quarterCount = 0;
static int clockOutCount = 0;

int main(int argc, char** argv)
{
    bool installCallbacks = true;
    float seconds = 10.f;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--no-callbacks")
            installCallbacks = false;
        else if (a.rfind("--seconds=", 0) == 0)
            seconds = std::stof(a.substr(10));
    }

    std::printf("Spotykach engine smoke test\n");
    std::printf("  callback installate: %s\n", installCallbacks ? "si" : "NO (crash atteso)");
    std::printf("  durata: %.1f s\n\n", seconds);

    BufferPool pool(42.f);
    std::printf("  pool allocato: %.1f MB\n", pool.bytes() / (1024.0 * 1024.0));

    Core core;

    if (installCallbacks) {
        core.driver().set_on_quarter([](const bool) { quarterCount++; });
        core.driver().set_on_clock_out([]() { clockOutCount++; });
    }

    core.init(kEngineSampleRate, (float)kEngineBlockSize, pool);
    core.set_route(Route::Stereo);
    for (auto ref : {Deck::A, Deck::B})
        core.deck(ref).set_mode(Mode::Reel);

    const int block = (int)kEngineBlockSize;
    const int blocks = (int)(seconds * kEngineSampleRate / block);

    float inL[kEngineBlockSize];
    float inR[kEngineBlockSize];
    float outL[kEngineBlockSize];
    float outR[kEngineBlockSize];
    float* inPtr[2] = {inL, inR};
    float* outPtr[2] = {outL, outR};

    // Registra sul deck A dal secondo 0, poi suona dal secondo 3.
    bool recStarted = false;
    bool playStarted = false;

    double peak = 0.0;
    int nonFinite = 0;
    uint32_t rng = 22222;

    for (int b = 0; b < blocks; b++) {
        const float t = (float)(b * block) / kEngineSampleRate;

        if (!recStarted && t >= 0.5f) {
            recStarted = true;
            core.set_source(Deck::Source::external, Deck::A);
            core.deck(Deck::A).toggle_recording();
            std::printf("  [%.1fs] rec ON deck A\n", t);
        }
        if (recStarted && !playStarted && t >= 3.0f) {
            playStarted = true;
            core.deck(Deck::A).toggle_recording();
            core.driver().toggle_play(Deck::A);
            std::printf("  [%.1fs] rec OFF, play ON deck A\n", t);
        }

        // Rumore rosa-ish come sorgente
        for (int i = 0; i < block; i++) {
            rng = rng * 1664525u + 1013904223u;
            const float n = ((int32_t)(rng >> 8) / 8388608.f - 1.f) * 0.3f;
            inL[i] = n;
            inR[i] = n * 0.8f;
        }

        core.driver().tick(false);
        core.prepare();
        core.process(inPtr, outPtr, block);

        for (int i = 0; i < block; i++) {
            float a = 0.f, bb = 0.f;
            core.mod(Deck::A).process(a);
            core.mod(Deck::B).process(bb);
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])
                || !std::isfinite(a) || !std::isfinite(bb))
                nonFinite++;
            peak = std::fmax(peak, std::fmax(std::fabs(outL[i]), std::fabs(outR[i])));
        }

        core.deck(Deck::A).voxs().read_reset_is_triggered();
        core.deck(Deck::B).voxs().read_reset_is_triggered();

        g_frames += block;
    }

    std::printf("\n  blocchi elaborati: %d\n", blocks);
    std::printf("  quarti: %d   clock out: %d\n", quarterCount, clockOutCount);
    std::printf("  picco uscita: %.4f\n", peak);
    std::printf("  campioni non finiti: %d\n", nonFinite);
    std::printf("  deck A playing: %s, buffer %.2f s\n",
                core.deck(Deck::A).is_playing() ? "si" : "no",
                core.deck(Deck::A).buffer().rec_size() / kEngineSampleRate);

    bool ok = (nonFinite == 0) && (peak > 1e-4) && (quarterCount > 0);
    std::printf("\n%s\n", ok ? "SMOKE TEST OK" : "SMOKE TEST FALLITO");

    // --- Regressione: cambiare velocità non deve fermare la riproduzione ----
    // _speed_mod_mult moltiplica l'incremento del playhead ogni volta che
    // l'incremento viene aggiornato, cosa che accade solo DOPO un cambio di
    // velocità. Se non è tenuto a 1 (via pitch_speed_mod_in, come fa read_cv
    // sull'hardware), l'audio si ferma appena si tocca il pitch.
    std::printf("\n--- test cambio velocita' durante il play ---\n");
    {
        double peakBefore = 0.0, peakAfter = 0.0;
        const int nb = (int)(1.0f * kEngineSampleRate / block);

        for (int b = 0; b < nb; b++) {
            core.driver().tick(false);
            core.prepare();
            core.process(inPtr, outPtr, block);
            for (int i = 0; i < block; i++)
                peakBefore = std::fmax(peakBefore, std::fabs(outL[i]));
            g_frames += block;
        }

        core.deck(Deck::A).voxs().set_speed(0.72f); // ~1.3x
        std::printf("  set_speed(0.72)\n");

        for (int b = 0; b < nb * 2; b++) {
            // Nota: NON chiamiamo pitch_speed_mod_in qui, per verificare che
            // il motore sia robusto anche senza — dopo il fix all'init di
            // _speed_mod_mult il default è 1.
            core.driver().tick(false);
            core.prepare();
            core.process(inPtr, outPtr, block);
            if (b >= nb) // salta la rampa di glide da 100 ms
                for (int i = 0; i < block; i++)
                    peakAfter = std::fmax(peakAfter, std::fabs(outL[i]));
            g_frames += block;
        }

        std::printf("  picco prima: %.5f   dopo: %.5f\n", peakBefore, peakAfter);
        const bool stillPlaying = (peakAfter > peakBefore * 0.2);
        std::printf("  %s\n", stillPlaying ? "PITCH OK" : "PITCH FALLITO: la riproduzione si e' fermata");
        ok = ok && stillPlaying;
    }

    // --- Isolamento fra istanze -------------------------------------------
    // È il rischio principale del porting: sull'hardware i buffer erano static
    // e condivisi. Se due istanze condividessero ancora qualcosa, la seconda
    // (che non registra e non suona) emetterebbe l'audio della prima.
    std::printf("\n--- test isolamento due istanze ---\n");
    {
        BufferPool poolX(4.f), poolY(4.f);
        Core coreX, coreY;
        for (Core* c : {&coreX, &coreY}) {
            c->driver().set_on_quarter([](const bool) {});
            c->driver().set_on_clock_out([]() {});
        }
        coreX.init(kEngineSampleRate, (float)kEngineBlockSize, poolX);
        coreY.init(kEngineSampleRate, (float)kEngineBlockSize, poolY);
        coreX.set_route(Route::Stereo);
        coreY.set_route(Route::Stereo);
        coreX.deck(Deck::A).set_mode(Mode::Reel);
        coreY.deck(Deck::A).set_mode(Mode::Reel);

        // Solo X registra e suona. Y resta inerte.
        coreX.set_source(Deck::Source::external, Deck::A);
        coreX.deck(Deck::A).toggle_recording();

        double peakX = 0.0, peakY = 0.0;
        float xL[kEngineBlockSize], xR[kEngineBlockSize];
        float yL[kEngineBlockSize], yR[kEngineBlockSize];
        float zero[kEngineBlockSize] = {};
        float* zeroPtr[2] = {zero, zero};
        float* xPtr[2] = {xL, xR};
        float* yPtr[2] = {yL, yR};

        const int n = (int)(3.f * kEngineSampleRate / kEngineBlockSize);
        for (int b = 0; b < n; b++) {
            for (int i = 0; i < block; i++) {
                rng = rng * 1664525u + 1013904223u;
                inL[i] = inR[i] = ((int32_t)(rng >> 8) / 8388608.f - 1.f) * 0.5f;
            }
            if (b == n / 2) {
                coreX.deck(Deck::A).toggle_recording();
                coreX.driver().toggle_play(Deck::A);
            }

            coreX.driver().tick(false);
            coreX.prepare();
            coreX.process(inPtr, xPtr, block);

            // Y riceve solo silenzio: qualunque suono in uscita sarebbe di X.
            coreY.driver().tick(false);
            coreY.prepare();
            coreY.process(zeroPtr, yPtr, block);

            for (int i = 0; i < block; i++) {
                peakX = std::fmax(peakX, std::fabs(xL[i]));
                peakY = std::fmax(peakY, std::fabs(yL[i]));
            }
            g_frames += block;
        }

        std::printf("  istanza X (registra e suona): picco %.5f\n", peakX);
        std::printf("  istanza Y (inerte, ingresso muto): picco %.5f\n", peakY);
        const bool isolated = (peakX > 1e-3) && (peakY < 1e-6);
        std::printf("  %s\n", isolated ? "ISOLAMENTO OK" : "ISOLAMENTO FALLITO: le istanze condividono stato");
        ok = ok && isolated;
    }

    return ok ? 0 : 1;
}
