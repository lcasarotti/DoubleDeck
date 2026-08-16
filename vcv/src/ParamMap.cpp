#include "ParamMap.hpp"

namespace dd
{

// --- Quantità che non hanno bisogno del motore ---------------------------
//
// Bastano gli altri parametri, quindi stanno qui e non in DoubleDeck.cpp.

bool ModSpeedQuantity::isSynced()
{
    return module && module->params[MOD_SYNC_A_PARAM + deck].getValue() > .5f;
}

std::string ModSpeedQuantity::getDisplayValueString()
{
    if (!isSynced()) {
        return string::f("%.2f", curvedValue(getValue()) * kLFOFreqRange + kLFOFreqMin);
    }

    // Agganciato al clock, `Modulator::set_speed_norm` sceglie una durata di
    // ciclo dentro `kFreqDiv` (in ticks di 1/4 di quarto): la stessa
    // indicizzazione, con la stessa saturazione, riportata qui in parole.
    static const std::array<const char*, 9> kCycles = {
        "1/8 beat", "1/4 beat", "1/2 beat", "1 beat", "2 beats",
        "1 bar",    "2 bars",   "3 bars",   "4 bars"
    };
    const int last = (int)kCycles.size() - 1;
    const int idx  = (int)(std::clamp(1.f - getValue(), 0.f, 1.f) * kCycles.size()) - 1;
    return kCycles[std::clamp(idx, 0, last)];
}

std::string ModSpeedQuantity::getUnit()
{
    return isSynced() ? "" : " Hz";
}

std::string CrossfadeQuantity::getDisplayValueString()
{
    const float v = getValue();
    if (v <= .001f) return "deck A only";
    if (v >= .999f) return "deck B only";
    if (std::abs(v - .5f) <= .005f) return "centre";
    return string::f("A %.0f%% / B %.0f%%", (1.f - v) * 100.f, v * 100.f);
}

std::string CrossfadeQuantity::getUnit()
{
    return "";
}

std::string QuartersQuantity::getDisplayValueString()
{
    const int q = (int)std::round(getValue());
    const std::string s = string::f("%d quarter%s", q, q == 1 ? "" : "s");
    if (q % 4 != 0) return s;
    const int bars = q / 4;
    return s + string::f(" (%d bar%s)", bars, bars == 1 ? "" : "s");
}

std::string QuartersQuantity::getUnit()
{
    return "";
}

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
                        {"Reel", "Slice", "Drift"})
            ->description = "Reel: one continuous loop. Slice: the loop cut into a grid,\n"
                            "played by triggers. Drift: overlapping grains.";
        m->configButton(TRIGGER_A_PARAM + d, "Trigger" + n)
            ->description = "Fires a voice by hand, like a pulse on the gate input.";

        // --- lettura del loop ----------------------------------------------
        auto* pos = m->configParam<PosQuantity>(POS_A_PARAM + d, 0.f, 1.f, 0.f,
                                                "Position" + n, "%", 0.f, 100.f);
        pos->deck = d;
        pos->description = "Where in the buffer the read head starts.";

        m->configSwitch(START_OFFSET_A_PARAM + d, 0.f, 8.f, 0.f, "Start offset" + n,
                        kOffsetLabels)
            ->description = "Walks the start position forward by one window every N\n"
                            "triggers, so a repeated slice moves through the loop.";

        auto* size = m->configParam<SizeQuantity>(SIZE_A_PARAM + d, 0.f, 1.f, 1.f,
                                                  "Size" + n, "%", 0.f, 100.f);
        size->deck = d;
        size->description = "Length of the slice read from the buffer. In Drift it is\n"
                            "the spread of the grains instead.";

        auto* pitch = m->configParam<PitchQuantity>(PITCH_A_PARAM + d, 0.f, 1.f, .5f,
                                                    "Pitch" + n);
        pitch->deck = d;
        pitch->description = "Unity at centre, 0 to 4x over the full travel. In Slice it\n"
                             "transposes without changing the length; elsewhere it is\n"
                             "read speed, so it shortens the slice as it raises it.";
        m->configSwitch(PITCH_QUANT_A_PARAM + d, 0.f, 1.f, 0.f, "Pitch quantize" + n,
                        {"Free", "Semitone steps"})
            ->description = "Snaps pitch to -24, -12, -7, 0, +7, +12, +24 semitones.";

        m->configParam(IO_MIX_A_PARAM + d, 0.f, 1.f, .5f, "In/out mix" + n, "%", 0.f, 100.f)
            ->description = "Balance between the live input and the loop, at the deck\n"
                            "output. Fully down is the dry input alone.";
        m->configParam(FEEDBACK_A_PARAM + d, 0.f, 1.f, .95f, "Overdub feedback" + n,
                       "%", 0.f, 100.f)
            ->description = "How much of the old take survives an overdub pass.";

        // --- grano (Drift) --------------------------------------------------
        m->configParam(ENV_SHAPE_A_PARAM + d, 0.f, 1.f, 0.f, "Envelope shape" + n,
                       "%", 0.f, 100.f)
            ->description = "Attack/decay tilt of the grain envelope. Below 5% in Drift\n"
                            "the voice stays silent.";
        m->configParam(ENV_SIZE_A_PARAM + d, 0.f, 1.f, 1.f, "Envelope size" + n,
                       "%", 0.f, 100.f)
            ->description = "Fraction of the grain the envelope covers.";
        // 60…500 ms, come Vox::set_win_size.
        m->configParam(WINDOW_A_PARAM + d, 0.f, 1.f, .2f, "Grain window" + n,
                       " ms", 0.f, 440.f, 60.f)
            ->description = "Length of a single grain. Drift only.";

        // --- CV --------------------------------------------------------------
        m->configSwitch(CV_DEST_A_PARAM + d, 0.f, 2.f, 1.f, "Size/pos CV target" + n,
                        {"Size", "Size and position", "Position"})
            ->description = "What the size/position CV input drives.";

        // --- modulatore -------------------------------------------------------
        m->configSwitch(MOD_TYPE_A_PARAM + d, 0.f, 1.f, 1.f, "Modulator type" + n,
                        {"Envelope follower", "LFO"})
            ->description = "Source of the modulation CV output.";
        // Sull'hardware le forme d'onda sono spartite fra i due deck
        // (A: square/random, B: saw/sine); qui entrambi le hanno tutte e quattro.
        m->configSwitch(LFO_SHAPE_A_PARAM + d, 0.f, 3.f, 0.f, "LFO shape" + n,
                        {"Sine", "Square", "Saw", "Random"});
        m->configSwitch(MOD_SYNC_A_PARAM + d, 0.f, 1.f, 0.f, "Modulator sync" + n,
                        {"Free running", "Clock synced"})
            ->description = "Synced, the LFO cycle is a division of the tempo and\n"
                            "restarts with it; free running, it is a rate in hertz.";

        auto* modSpeed = m->configParam<ModSpeedQuantity>(MOD_SPEED_A_PARAM + d, 0.f, 1.f, .3f,
                                                          "Modulator speed" + n);
        modSpeed->deck = d;
        modSpeed->description = "LFO rate, or follower response time.";

        m->configParam(MOD_AMOUNT_A_PARAM + d, 0.f, 1.f, 0.f, "Modulator amount" + n,
                       "%", 0.f, 100.f)
            ->description = "Depth of the modulation CV output.";

        // --- Grit --------------------------------------------------------------
        m->configSwitch(GRIT_ON_A_PARAM + d, 0.f, 1.f, 0.f, "Grit on" + n,
                        {"Off", "On"});
        m->configSwitch(GRIT_MODE_A_PARAM + d, 0.f, 1.f, 0.f, "Grit mode" + n,
                        {"Drive", "Reduce"})
            ->description = "Drive: overdrive. Reduce: bit and rate crushing.\n"
                            "Intensity and mix are remembered per mode on the hardware;\n"
                            "here the two knobs always say what is in force.";
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
                        {"Off", "Armed / recording"})
            ->description = "Records the triggers you play into a bar-long grid, which\n"
                            "then fires them back. Disarming lets the pass finish.";
        m->configButton(SEQ_CLEAR_A_PARAM + d, "Sequence clear" + n)
            ->description = "Empties the recorded sequence.";

        // --- tempo dal loop --------------------------------------------------------
        auto* quarters = m->configParam<QuartersQuantity>(SIZE_QUARTERS_A_PARAM + d,
                                                          1.f, 16.f, 4.f, "Loop length" + n);
        quarters->snapEnabled   = true;
        quarters->smoothEnabled = false;
        quarters->description   = "How many quarters the recorded loop is meant to span.\n"
                                  "Read by the fit-tempo button, which does the rest.";
        m->configButton(FIT_TEMPO_A_PARAM + d, "Fit tempo to loop" + n)
            ->description = "Sets the tempo so the loop lasts exactly the length above.";
    }

    // --- Globali ---------------------------------------------------------------
    m->configParam<CrossfadeQuantity>(CROSSFADE_PARAM, 0.f, 1.f, .5f, "Crossfade A/B")
        ->description = "Balance between the two decks at the main output.\n"
                        "The crossfade CV can only push it towards deck B.";
    m->configParam(TEMPO_PARAM, 20.f, 250.f, 120.f, "Tempo", " BPM")
        ->description = "Internal tempo. Under external clock it follows the clock\n"
                        "instead, and the knob only reports it.";
    m->configButton(TAP_PARAM, "Tap tempo")
        ->description = "Taps the internal tempo. Ignored under external clock.";

    auto* key = m->configParam<KeyIntervalQuantity>(KEY_INTERVAL_PARAM, 0.f, 16.f, 1.f,
                                                    "Key interval");
    key->snapEnabled   = true;
    key->smoothEnabled = false;
    key->description   = "The grid that quantized starts and stops wait for: play,\n"
                         "record and the sequencer all land on a key tick.";

    m->configSwitch(CLOCK_SOURCE_PARAM, 0.f, 1.f, 0.f, "Clock source",
                    {"Internal", "External"})
        ->description = "External follows the clock input, and falls back to internal\n"
                        "one second after the pulses stop.";

    // Il default è uno **per quarto**, che è la convenzione più diffusa fra i
    // moduli di clock di Rack. L'hardware usa 4 sul suo jack di clock: chi
    // arriva da lì, o da un clock in stile eurorack, sposta di tre scatti.
    auto* ppqn = m->configSwitch(CLOCK_PPQN_PARAM, 0.f, (float)kClockPPQN.size() - 1, 0.f,
                                 "Clock in PPQN",
                                 {"1 (quarters)", "2 (8ths)", "3 (quarter triplets)",
                                  "4 (16ths)", "6 (8th triplets)", "8 (32nds)",
                                  "12 (16th triplets)", "16", "24 (MIDI)", "48"});
    ppqn->snapEnabled   = true;
    ppqn->smoothEnabled = false;
    ppqn->description   = "How many pulses per quarter note the clock input carries.\n"
                          "Get it wrong and the module runs at the wrong tempo: at\n"
                          "4 with a one-pulse-per-beat clock it plays four times slow.";
    m->configButton(CLOCK_RESET_PARAM, "Clock reset")
        ->description = "Puts the clock back on the downbeat.";
    m->configSwitch(ROUTE_PARAM, 0.f, 2.f, 1.f, "Route",
                    {"Double mono", "Stereo", "Generative stereo"})
        ->description = "How the inputs feed the decks and how the decks reach the\n"
                        "output: it also sets the panner and the voice width.";
    m->configParam(CLICK_MIX_PARAM, 0.f, 1.f, 0.f, "Click level", "%", 0.f, 100.f)
        ->description = "Metronome on the quarters, mixed into the main output.";
    m->configParam(PAN_SPEED_PARAM, 0.f, 1.f, 1.f, "Pan speed", "%", 0.f, 100.f)
        ->description = "Rate of the automatic panner. Route decides its shape.";
    m->configParam(PAN_RANGE_PARAM, 0.f, 1.f, .6f, "Pan range", "%", 0.f, 95.f, 5.f)
        ->description = "How far off centre the panner travels.";

    // --- Porte -------------------------------------------------------------------
    m->configInput(IN_L_INPUT, "Audio left")->description = "Normalled to the right input.";
    m->configInput(IN_R_INPUT, "Audio right");
    m->configInput(CV_CROSSFADE_INPUT, "Crossfade CV")
        ->description = "Positive voltage only: it pushes the crossfade towards deck B.";
    m->configInput(CLOCK_INPUT, "Clock")->description = "Used when the clock source is external.";
    m->configOutput(OUT_L_OUTPUT, "Audio left");
    m->configOutput(OUT_R_OUTPUT, "Audio right");
    m->configOutput(CLOCK_OUTPUT, "Clock (24 PPQN)");

    for (int d = 0; d < 2; d++) {
        const std::string n = d == 0 ? ", deck A" : ", deck B";
        m->configInput(CV_SIZE_POS_A_INPUT + d, "Size/position CV" + n)
            ->description = "Target chosen by the size/pos CV target switch.";
        m->configInput(CV_MIX_A_INPUT + d, "In/out mix CV" + n);
        m->configInput(VOCT_A_INPUT + d, "V/oct" + n)
            ->description = "Plus or minus five octaves around the pitch knob.";
        m->configInput(GATE_A_INPUT + d, "Gate in" + n)
            ->description = "Fires a voice; the V/oct reading travels with it.";
        m->configOutput(GATE_A_OUTPUT + d, "Gate out" + n)
            ->description = "A 7 ms pulse each time a voice restarts.";
        m->configOutput(MOD_A_OUTPUT + d, "Modulation CV" + n)
            ->description = "The modulator of this deck, 0 to 10 V.";
    }

    m->configBypass(IN_L_INPUT, OUT_L_OUTPUT);
    m->configBypass(IN_R_INPUT, OUT_R_OUTPUT);
}

} // namespace dd
