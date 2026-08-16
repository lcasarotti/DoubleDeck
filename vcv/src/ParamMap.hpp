// La superficie di controllo del modulo: identificatori, dichiarazione dei
// parametri e formattazione dei valori.
//
// Rimpiazza `CoreUI` del firmware. Sull'hardware ventidue potenziometri servono
// una settantina di funzioni tramite cinque layer (BASE / ALT / TAP-HOLD /
// FLUX / GRIT); qui ogni funzione ha il suo parametro dedicato, quindi non
// serve né il layering né la logica di pickup di `MValue`.
//
// Le coppie A/B sono **adiacenti** in tutti gli enum: `X_A_PARAM + deck`
// indirizza il deck giusto, e non serve nessuna tabella di lookup.
//
// L'etichetta e l'unità di ogni parametro non sono decorazione: in MetaRack la
// vista PARAM legge esattamente questi campi dalla `ParamQuantity`, quindi sono
// l'interfaccia utente primaria per chi usa uno screen reader.
#pragma once

#include <rack.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace dd
{

using namespace rack;

enum ParamId {
    // --- Per deck (coppie A/B adiacenti) ---------------------------------
    PLAY_A_PARAM, PLAY_B_PARAM,
    REC_A_PARAM, REC_B_PARAM,
    REC_SOURCE_A_PARAM, REC_SOURCE_B_PARAM,
    REVERSE_A_PARAM, REVERSE_B_PARAM,
    MODE_A_PARAM, MODE_B_PARAM,
    TRIGGER_A_PARAM, TRIGGER_B_PARAM,
    POS_A_PARAM, POS_B_PARAM,
    START_OFFSET_A_PARAM, START_OFFSET_B_PARAM,
    SIZE_A_PARAM, SIZE_B_PARAM,
    PITCH_A_PARAM, PITCH_B_PARAM,
    PITCH_QUANT_A_PARAM, PITCH_QUANT_B_PARAM,
    IO_MIX_A_PARAM, IO_MIX_B_PARAM,
    FEEDBACK_A_PARAM, FEEDBACK_B_PARAM,
    ENV_SHAPE_A_PARAM, ENV_SHAPE_B_PARAM,
    ENV_SIZE_A_PARAM, ENV_SIZE_B_PARAM,
    WINDOW_A_PARAM, WINDOW_B_PARAM,
    CV_DEST_A_PARAM, CV_DEST_B_PARAM,
    MOD_TYPE_A_PARAM, MOD_TYPE_B_PARAM,
    LFO_SHAPE_A_PARAM, LFO_SHAPE_B_PARAM,
    MOD_SYNC_A_PARAM, MOD_SYNC_B_PARAM,
    MOD_SPEED_A_PARAM, MOD_SPEED_B_PARAM,
    MOD_AMOUNT_A_PARAM, MOD_AMOUNT_B_PARAM,
    GRIT_ON_A_PARAM, GRIT_ON_B_PARAM,
    GRIT_MODE_A_PARAM, GRIT_MODE_B_PARAM,
    GRIT_INTENS_A_PARAM, GRIT_INTENS_B_PARAM,
    GRIT_MIX_A_PARAM, GRIT_MIX_B_PARAM,
    FLUX_ON_A_PARAM, FLUX_ON_B_PARAM,
    FLUX_INTENS_A_PARAM, FLUX_INTENS_B_PARAM,
    FLUX_FB_A_PARAM, FLUX_FB_B_PARAM,
    FLUX_MIX_A_PARAM, FLUX_MIX_B_PARAM,
    SEQ_ARM_A_PARAM, SEQ_ARM_B_PARAM,
    SEQ_CLEAR_A_PARAM, SEQ_CLEAR_B_PARAM,
    SIZE_QUARTERS_A_PARAM, SIZE_QUARTERS_B_PARAM,
    FIT_TEMPO_A_PARAM, FIT_TEMPO_B_PARAM,
    // --- Globali ---------------------------------------------------------
    CROSSFADE_PARAM,
    TEMPO_PARAM,
    TAP_PARAM,
    KEY_INTERVAL_PARAM,
    CLOCK_SOURCE_PARAM,
    CLOCK_RESET_PARAM,
    ROUTE_PARAM,
    CLICK_MIX_PARAM,
    PAN_SPEED_PARAM,
    PAN_RANGE_PARAM,
    // Fuori dal gruppo del clock, a cui appartiene, perché Rack salva i
    // parametri per indice: aggiungerlo in mezzo cambierebbe di posto tutti
    // quelli sotto e le patch già salvate leggerebbero i valori sbagliati.
    CLOCK_PPQN_PARAM,
    PARAMS_LEN
};

enum InputId {
    IN_L_INPUT, IN_R_INPUT,
    CV_SIZE_POS_A_INPUT, CV_SIZE_POS_B_INPUT,
    CV_MIX_A_INPUT, CV_MIX_B_INPUT,
    VOCT_A_INPUT, VOCT_B_INPUT,
    GATE_A_INPUT, GATE_B_INPUT,
    CV_CROSSFADE_INPUT,
    CLOCK_INPUT,
    INPUTS_LEN
};

enum OutputId {
    OUT_L_OUTPUT, OUT_R_OUTPUT,
    GATE_A_OUTPUT, GATE_B_OUTPUT,
    MOD_A_OUTPUT, MOD_B_OUTPUT,
    CLOCK_OUTPUT,
    OUTPUTS_LEN
};

enum LightId {
    PLAY_A_LIGHT, PLAY_B_LIGHT,
    REC_A_LIGHT, REC_B_LIGHT,
    SEQ_A_LIGHT, SEQ_B_LIGHT,
    GRIT_A_LIGHT, GRIT_B_LIGHT,
    FLUX_A_LIGHT, FLUX_B_LIGHT,
    GATE_IN_A_LIGHT, GATE_IN_B_LIGHT,
    MOD_A_LIGHT, MOD_B_LIGHT,
    CLOCK_LIGHT,
    LIGHTS_LEN
};

// Posizioni del knob PITCH corrispondenti ai sette intervalli quantizzati
// (−24, −12, −7, 0, +7, +12, +24 semitoni). Copiate da `kSpeedSteps` in
// src/ui/core.ui.h: sono matematica pura, e src/ui/ non è compilabile qui.
static constexpr std::array<float, 7> kSpeedSteps = {
    .125f,        // −24
    .25f,         // −12
    .33371f,      // −7
    .5f,          //   0
    .5830516667f, //  +7
    .666666667f,  // +12
    1.f           // +24
};

/// Il rapporto di velocità che il motore ricava dal valore del knob:
/// 0…0,5 → 0…1×, 0,5…1 → 1…4×. Unità esatta a metà corsa.
/// Copia di `mapped_speed` in src/core/generator.cpp.
inline float mappedSpeed(const float v)
{
    return v < .5f ? 2.f * v : 1.f + (v - .5f) * 6.f;
}

/// Aggancio al passo quantizzato più vicino, come `snapped_speed` in
/// src/ui/core.ui.cpp.
inline float snappedSpeed(const float v)
{
    const auto last = static_cast<float>(kSpeedSteps.size() - 1);
    const auto idx  = static_cast<int>(std::clamp(std::round(v * last), 0.f, last));
    return kSpeedSteps[idx];
}

/// La curva che il motore applica a velocità e ampiezza del modulatore.
/// Copia di `curved_value` in src/core/lfo.h, che qui non è includibile perché
/// tira dentro daisysp.h.
inline float curvedValue(float norm)
{
    norm = norm < .5f ? 1.5f * norm / (norm + 1.f) : norm * norm * .6666666667f + .334f;
    return std::clamp(norm, 0.f, 1.f);
}

/// Estremi della frequenza dell'LFO libero, da src/core/config.h.
static constexpr float kLFOFreqMin   = .01f;
static constexpr float kLFOFreqRange = 11.99f;

/// Impulsi per quarto accettabili sull'ingresso di clock. Solo divisori di 48
/// (`kPPQNIntern`): `SynClock::SetPPQNIn` calcola 48 / ppqn in aritmetica
/// intera, e un valore che non divide falserebbe la conversione.
static constexpr std::array<int, 10> kClockPPQN = {1, 2, 3, 4, 6, 8, 12, 16, 24, 48};

// --- Quantità con formattazione propria ---------------------------------
//
// Sono i valori che, letti come percentuale, non direbbero niente: intonazione,
// intervallo di key tick, lunghezza del loop.

/// PITCH: mostra rapporto e semitoni, tenendo conto dello switch di
/// quantizzazione (che è ciò che il motore applica davvero).
struct PitchQuantity : ParamQuantity {
    int deck = 0;

    std::string getDisplayValueString() override
    {
        auto v = getValue();
        if (module && module->params[PITCH_QUANT_A_PARAM + deck].getValue() > .5f) {
            v = snappedSpeed(v);
        }
        const auto ratio = mappedSpeed(v);
        if (ratio < 1e-4f) return "stopped";
        return string::f("%.2fx (%+.1f st)", ratio, 12.f * std::log2(ratio));
    }
};

/// KEY INTERVAL: i 17 passi di `kKeyIntervals` in src/core/driver.h, da 1/16 a
/// 4 battute.
struct KeyIntervalQuantity : ParamQuantity {
    std::string getDisplayValueString() override
    {
        const int idx = static_cast<int>(std::round(getValue()));
        if (idx <= 0) return "1/16";
        if (idx % 4 == 0) {
            const int bars = idx / 4;
            return string::f("%d/4 (%d bar%s)", idx, bars, bars > 1 ? "s" : "");
        }
        return string::f("%d/4", idx);
    }
};

/// SIZE: in percentuale non dice molto; con un loop registrato la lunghezza in
/// secondi sì. L'unità segue il valore, altrimenti `Quantity::getString()`
/// appiccica il "%" ai secondi. Il corpo sta in DoubleDeck.cpp, dove il tipo
/// del modulo è noto.
struct SizeQuantity : ParamQuantity {
    int deck = 0;
    std::string getDisplayValueString() override;
    std::string getUnit() override;

    /// Lunghezza della finestra in secondi, 0 se il deck è vuoto.
    float seconds();
};

/// POS: come SIZE, la percentuale è un numero senza riferimento. Con del
/// materiale nel buffer diventa "a che secondo attacca la lettura".
struct PosQuantity : ParamQuantity {
    int deck = 0;
    std::string getDisplayValueString() override;
    std::string getUnit() override;

    /// Posizione di partenza in secondi, 0 se il deck è vuoto.
    float seconds();
};

/// MOD SPEED: due scale diverse a seconda dello switch di sync — hertz se
/// l'LFO corre libero, suddivisione del tempo se è agganciato al clock. Una
/// percentuale non direbbe né l'una né l'altra.
struct ModSpeedQuantity : ParamQuantity {
    int deck = 0;
    bool isSynced();
    std::string getDisplayValueString() override;
    std::string getUnit() override;
};

/// CROSSFADE: 0 è il solo deck A, 1 il solo deck B. Detto in percentuale
/// secca resta ambiguo su *quale* dei due sta salendo.
struct CrossfadeQuantity : ParamQuantity {
    std::string getDisplayValueString() override;
    std::string getUnit() override;
};

/// LOOP LENGTH: quarti, più le battute quando il conto torna. Serve anche a
/// non far dire "1 quarters" all'unità fissa.
struct QuartersQuantity : ParamQuantity {
    std::string getDisplayValueString() override;
    std::string getUnit() override;
};

/// Dichiara l'intera superficie. Chiamata dal costruttore del modulo.
void configureAll(Module* m);

} // namespace dd
