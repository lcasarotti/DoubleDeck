#include "ParamMap.hpp"

namespace dd
{

// I valori di default ricalcano `CoreUI::_init_values()` e i costruttori di
// `Fx` / `Drive`, così un modulo appena aggiunto suona come lo Spotykach appena
// acceso.
void configureAll(Module* m)
{
    static const std::vector<std::string> kOffsetLabels = {
        "Off",
        "Every trigger",
        "Every 2 triggers",
        "Every 3 triggers",
        "Every 4 triggers",
        "Every 5 triggers",
        "Every 6 triggers",
        "Every 7 triggers",
        "Every 8 triggers"
    };

    // Il riferimento al deck va **in coda**, non in testa: nella vista PARAM si
    // cerca un parametro digitandone l'iniziale, e con "Deck A …" ogni nome
    // comincerebbe per D. Così l'iniziale è quella del comando, e i due deck
    // restano comunque adiacenti nell'ordine alfabetico.
    for (int d = 0; d < 2; d++) {
        const std::string n = d == 0 ? ", deck A" : ", deck B";

        // --- trasporto -----------------------------------------------------
        m->configSwitch(PLAY_A_PARAM + d, 0.f, 1.f, 0.f, "Play" + n,
                        {"Stopped", "Playing"});
        m->configSwitch(REC_A_PARAM + d, 0.f, 1.f, 0.f, "Record" + n,
                        {"Off", "Armed / recording"});
        m->configSwitch(REC_SOURCE_A_PARAM + d, 0.f, 1.f, 0.f, "Record source" + n,
                        {"External input", "Other deck"});
        // Sull'hardware Play e Rev sono due pad distinti; qui la direzione è uno
        // switch a sé, che conserva la semantica originale: cambiarla mentre il
        // deck suona inverte senza fermare.
        m->configSwitch(REVERSE_A_PARAM + d, 0.f, 1.f, 0.f, "Direction" + n,
                        {"Forward", "Reverse"});
        m->configSwitch(MODE_A_PARAM + d, 0.f, 2.f, 0.f, "Mode" + n,
                        {"Reel", "Slice", "Drift"});
        m->configButton(TRIGGER_A_PARAM + d, "Trigger" + n);

        // --- lettura del loop ----------------------------------------------
        m->configParam(POS_A_PARAM + d, 0.f, 1.f, 0.f, "Position" + n, "%", 0.f, 100.f);
        m->configSwitch(START_OFFSET_A_PARAM + d, 0.f, 8.f, 0.f, "Start offset" + n,
                        kOffsetLabels);

        auto* size = m->configParam<SizeQuantity>(SIZE_A_PARAM + d, 0.f, 1.f, 1.f,
                                                  "Size" + n, "%", 0.f, 100.f);
        size->deck = d;

        auto* pitch = m->configParam<PitchQuantity>(PITCH_A_PARAM + d, 0.f, 1.f, .5f,
                                                    "Pitch" + n);
        pitch->deck = d;
        m->configSwitch(PITCH_QUANT_A_PARAM + d, 0.f, 1.f, 0.f, "Pitch quantize" + n,
                        {"Free", "Semitone steps"});

        m->configParam(IO_MIX_A_PARAM + d, 0.f, 1.f, .5f, "In/out mix" + n, "%", 0.f, 100.f);
        m->configParam(FEEDBACK_A_PARAM + d, 0.f, 1.f, .95f, "Overdub feedback" + n,
                       "%", 0.f, 100.f);

        // --- grano (Drift) --------------------------------------------------
        m->configParam(ENV_SHAPE_A_PARAM + d, 0.f, 1.f, 0.f, "Envelope shape" + n,
                       "%", 0.f, 100.f);
        m->configParam(ENV_SIZE_A_PARAM + d, 0.f, 1.f, 1.f, "Envelope size" + n,
                       "%", 0.f, 100.f);
        // 60…500 ms, come Vox::set_win_size.
        m->configParam(WINDOW_A_PARAM + d, 0.f, 1.f, .2f, "Grain window" + n,
                       " ms", 0.f, 440.f, 60.f);

        // --- CV --------------------------------------------------------------
        m->configSwitch(CV_DEST_A_PARAM + d, 0.f, 2.f, 1.f, "Size/pos CV target" + n,
                        {"Size", "Size and position", "Position"});

        // --- modulatore -------------------------------------------------------
        m->configSwitch(MOD_TYPE_A_PARAM + d, 0.f, 1.f, 1.f, "Modulator type" + n,
                        {"Envelope follower", "LFO"});
        // Sull'hardware le forme d'onda sono spartite fra i due deck
        // (A: square/random, B: saw/sine); qui entrambi le hanno tutte e quattro.
        m->configSwitch(LFO_SHAPE_A_PARAM + d, 0.f, 3.f, 0.f, "LFO shape" + n,
                        {"Sine", "Square", "Saw", "Random"});
        m->configSwitch(MOD_SYNC_A_PARAM + d, 0.f, 1.f, 0.f, "Modulator sync" + n,
                        {"Free running", "Clock synced"});
        m->configParam(MOD_SPEED_A_PARAM + d, 0.f, 1.f, .3f, "Modulator speed" + n,
                       "%", 0.f, 100.f);
        m->configParam(MOD_AMOUNT_A_PARAM + d, 0.f, 1.f, 0.f, "Modulator amount" + n,
                       "%", 0.f, 100.f);

        // --- Grit --------------------------------------------------------------
        m->configSwitch(GRIT_ON_A_PARAM + d, 0.f, 1.f, 0.f, "Grit on" + n,
                        {"Off", "On"});
        m->configSwitch(GRIT_MODE_A_PARAM + d, 0.f, 1.f, 0.f, "Grit mode" + n,
                        {"Drive", "Reduce"});
        m->configParam(GRIT_INTENS_A_PARAM + d, 0.f, 1.f, .2f, "Grit intensity" + n,
                       "%", 0.f, 100.f);
        m->configParam(GRIT_MIX_A_PARAM + d, 0.f, 1.f, .33f, "Grit mix" + n,
                       "%", 0.f, 100.f);

        // --- Flux ---------------------------------------------------------------
        m->configSwitch(FLUX_ON_A_PARAM + d, 0.f, 1.f, 0.f, "Flux on" + n,
                        {"Off", "On"});
        // Tempo di ritardo dell'eco: 0,01…2 s (Fx::_apply_flux_int).
        m->configParam(FLUX_INTENS_A_PARAM + d, 0.f, 1.f, .5f, "Flux time" + n,
                       " s", 0.f, 1.99f, .01f);
        m->configParam(FLUX_FB_A_PARAM + d, 0.f, 1.f, .5f, "Flux feedback" + n,
                       "%", 0.f, 100.f);
        // Il mix del Flux è in dB, −40…0 (Fx::_apply_flux_mix).
        m->configParam(FLUX_MIX_A_PARAM + d, 0.f, 1.f, .7f, "Flux mix" + n,
                       " dB", 0.f, 40.f, -40.f);

        // --- sequencer ------------------------------------------------------------
        m->configSwitch(SEQ_ARM_A_PARAM + d, 0.f, 1.f, 0.f, "Sequence arm" + n,
                        {"Off", "Armed / recording"});
        m->configButton(SEQ_CLEAR_A_PARAM + d, "Sequence clear" + n);

        // --- tempo dal loop --------------------------------------------------------
        auto* quarters = m->configParam(SIZE_QUARTERS_A_PARAM + d, 1.f, 16.f, 4.f,
                                        "Loop length" + n, " quarters");
        quarters->snapEnabled   = true;
        quarters->smoothEnabled = false;
        m->configButton(FIT_TEMPO_A_PARAM + d, "Fit tempo to loop" + n);
    }

    // --- Globali ---------------------------------------------------------------
    m->configParam(CROSSFADE_PARAM, 0.f, 1.f, .5f, "Crossfade A/B", "%", 0.f, 100.f);
    m->configParam(TEMPO_PARAM, 20.f, 250.f, 120.f, "Tempo", " BPM");
    m->configButton(TAP_PARAM, "Tap tempo");

    auto* key = m->configParam<KeyIntervalQuantity>(KEY_INTERVAL_PARAM, 0.f, 16.f, 1.f,
                                                    "Key interval");
    key->snapEnabled   = true;
    key->smoothEnabled = false;

    m->configSwitch(CLOCK_SOURCE_PARAM, 0.f, 1.f, 0.f, "Clock source",
                    {"Internal", "External"});
    m->configButton(CLOCK_RESET_PARAM, "Clock reset");
    m->configSwitch(ROUTE_PARAM, 0.f, 2.f, 1.f, "Route",
                    {"Double mono", "Stereo", "Generative stereo"});
    m->configParam(CLICK_MIX_PARAM, 0.f, 1.f, 0.f, "Click level", "%", 0.f, 100.f);
    m->configParam(PAN_SPEED_PARAM, 0.f, 1.f, 1.f, "Pan speed", "%", 0.f, 100.f);
    m->configParam(PAN_RANGE_PARAM, 0.f, 1.f, .6f, "Pan range", "%", 0.f, 95.f, 5.f);

    // --- Porte -------------------------------------------------------------------
    m->configInput(IN_L_INPUT, "Audio left");
    m->configInput(IN_R_INPUT, "Audio right");
    m->configInput(CV_CROSSFADE_INPUT, "Crossfade CV");
    m->configInput(CLOCK_INPUT, "Clock");
    m->configOutput(OUT_L_OUTPUT, "Audio left");
    m->configOutput(OUT_R_OUTPUT, "Audio right");
    m->configOutput(CLOCK_OUTPUT, "Clock (24 PPQN)");

    for (int d = 0; d < 2; d++) {
        const std::string n = d == 0 ? ", deck A" : ", deck B";
        m->configInput(CV_SIZE_POS_A_INPUT + d, "Size/position CV" + n);
        m->configInput(CV_MIX_A_INPUT + d, "In/out mix CV" + n);
        m->configInput(VOCT_A_INPUT + d, "V/oct" + n);
        m->configInput(GATE_A_INPUT + d, "Gate in" + n);
        m->configOutput(GATE_A_OUTPUT + d, "Gate out" + n);
        m->configOutput(MOD_A_OUTPUT + d, "Modulation CV" + n);
    }

    m->configBypass(IN_L_INPUT, OUT_L_OUTPUT);
    m->configBypass(IN_R_INPUT, OUT_R_OUTPUT);
}

} // namespace dd
