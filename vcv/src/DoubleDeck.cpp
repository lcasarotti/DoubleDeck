#include "plugin.hpp"

#include "buffer_pool.h"
#include "core/core.h"
#include "core/event.h"
#include "core/mode.h"
#include "ui/speed.map.h" // matematica pura: 2^(semitoni/12), riusata dal firmware

using namespace spotykach;

// Il motore gira sempre a 48 kHz in blocchi da 96 campioni (2 ms), esattamente
// come sull'hardware: le costanti DSP del firmware sono tarate su quel rate e
// non vengono toccate. L'adattamento al sample rate di Rack avviene con due
// SampleRateConverter, bypassati quando Rack gira già a 48 kHz.
static constexpr int kBlock = (int)spotykach::kEngineBlockSize;
static constexpr float kEngineSR = spotykach::kEngineSampleRate;

struct DoubleDeckModule : Module {
    enum ParamId {
        // Deck A
        PLAY_A_PARAM,
        REC_A_PARAM,
        MODE_A_PARAM,
        POS_A_PARAM,
        SIZE_A_PARAM,
        PITCH_A_PARAM,
        IO_MIX_A_PARAM,
        // Deck B
        PLAY_B_PARAM,
        REC_B_PARAM,
        MODE_B_PARAM,
        POS_B_PARAM,
        SIZE_B_PARAM,
        PITCH_B_PARAM,
        IO_MIX_B_PARAM,
        // Globali
        CROSSFADE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        IN_L_INPUT,
        IN_R_INPUT,
        GATE_A_INPUT,
        GATE_B_INPUT,
        VOCT_A_INPUT,
        VOCT_B_INPUT,
        CLOCK_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        OUT_L_OUTPUT,
        OUT_R_OUTPUT,
        GATE_A_OUTPUT,
        GATE_B_OUTPUT,
        MOD_A_OUTPUT,
        MOD_B_OUTPUT,
        CLOCK_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        PLAY_A_LIGHT,
        PLAY_B_LIGHT,
        REC_A_LIGHT,
        REC_B_LIGHT,
        CLOCK_LIGHT,
        LIGHTS_LEN
    };

    BufferPool pool;
    Core core;

    dsp::SampleRateConverter<2> inputSrc;
    dsp::SampleRateConverter<4> outputSrc;
    dsp::DoubleRingBuffer<dsp::Frame<2>, 256> inputBuffer;
    dsp::DoubleRingBuffer<dsp::Frame<4>, 256> outputBuffer;

    // Ultimo valore noto dei latch, per distinguere un'azione dell'utente da un
    // riallineamento nostro.
    bool playShadow[2] = {false, false};
    bool recShadow[2] = {false, false};
    dsp::SchmittTrigger gateTrigger[2];
    dsp::SchmittTrigger clockTrigger;
    dsp::PulseGenerator gateOutPulse[2];
    dsp::PulseGenerator clockOutPulse;
    dsp::PulseGenerator quarterPulse;

    // I gate in sono campionati per campione (Rack è sample-accurate) e
    // consumati a fine blocco, dove l'hardware li leggeva a 500 Hz.
    bool gateLatched[2] = {false, false};
    bool clockHigh = false;
    bool prevClockHigh = false;

    // ±5 ottave, come l'hardware (SpeedMap<60> in CoreUI).
    SpeedMap<60> speedMap;
    float speedMult[2] = {1.f, 1.f};

    DoubleDeckModule() : pool(42.f) {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        const char* deckName[2] = {"Deck A", "Deck B"};
        const int playP[2] = {PLAY_A_PARAM, PLAY_B_PARAM};
        const int recP[2] = {REC_A_PARAM, REC_B_PARAM};
        const int modeP[2] = {MODE_A_PARAM, MODE_B_PARAM};
        const int posP[2] = {POS_A_PARAM, POS_B_PARAM};
        const int sizeP[2] = {SIZE_A_PARAM, SIZE_B_PARAM};
        const int pitchP[2] = {PITCH_A_PARAM, PITCH_B_PARAM};
        const int mixP[2] = {IO_MIX_A_PARAM, IO_MIX_B_PARAM};

        for (int d = 0; d < 2; d++) {
            configSwitch(playP[d], 0.f, 1.f, 0.f, string::f("%s play", deckName[d]),
                         {"Stopped", "Playing"});
            configSwitch(recP[d], 0.f, 1.f, 0.f, string::f("%s record", deckName[d]),
                         {"Off", "Armed / recording"});
            configSwitch(modeP[d], 0.f, 2.f, 0.f, string::f("%s mode", deckName[d]),
                         {"Reel", "Slice", "Drift"});
            configParam(posP[d], 0.f, 1.f, 0.f, string::f("%s position", deckName[d]), "%", 0.f, 100.f);
            configParam(sizeP[d], 0.f, 1.f, 1.f, string::f("%s size", deckName[d]), "%", 0.f, 100.f);
            configParam(pitchP[d], 0.f, 1.f, 0.5f, string::f("%s pitch", deckName[d]));
            configParam(mixP[d], 0.f, 1.f, 0.5f, string::f("%s in/out mix", deckName[d]), "%", 0.f, 100.f);
        }

        configParam(CROSSFADE_PARAM, 0.f, 1.f, 0.5f, "Crossfade A/B", "%", 0.f, 100.f);

        configInput(IN_L_INPUT, "Audio left");
        configInput(IN_R_INPUT, "Audio right");
        configInput(GATE_A_INPUT, "Deck A gate");
        configInput(GATE_B_INPUT, "Deck B gate");
        configInput(VOCT_A_INPUT, "Deck A V/oct");
        configInput(VOCT_B_INPUT, "Deck B V/oct");
        configInput(CLOCK_INPUT, "Clock");

        configOutput(OUT_L_OUTPUT, "Audio left");
        configOutput(OUT_R_OUTPUT, "Audio right");
        configOutput(GATE_A_OUTPUT, "Deck A gate");
        configOutput(GATE_B_OUTPUT, "Deck B gate");
        configOutput(MOD_A_OUTPUT, "Deck A modulation CV");
        configOutput(MOD_B_OUTPUT, "Deck B modulation CV");
        configOutput(CLOCK_OUTPUT, "Clock (24 PPQN)");

        configBypass(IN_L_INPUT, OUT_L_OUTPUT);
        configBypass(IN_R_INPUT, OUT_R_OUTPUT);

        initEngine();
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        initEngine();
    }

    void initEngine() {
        speedMap.init();
        playShadow[0] = playShadow[1] = false;
        recShadow[0] = recShadow[1] = false;

        // Driver::_on_quarter e _on_clock_out sono std::function che
        // sull'hardware installa CoreUI::init. Senza, la prima chiamata da
        // _send_tick lancia std::bad_function_call: nel thread audio non c'è
        // nessuno a catturarla, quindi terminate() e crash di Rack.
        // Vanno installate PRIMA di Core::init.
        core.driver().set_on_quarter([this](const bool is_key) {
            quarterPulse.trigger(is_key ? 0.08f : 0.04f);
        });
        core.driver().set_on_clock_out([this]() {
            clockOutPulse.trigger(1e-3f);
        });

        core.init(kEngineSR, (float)kBlock, pool);
        core.set_route(Route::Stereo);
        for (auto ref : {Deck::A, Deck::B}) {
            core.deck(ref).set_mode(Mode::Reel);
        }
    }

    /// Ciò che sull'hardware sta in CoreUI::process + read_cv: gira una volta
    /// per blocco, cioè a 500 Hz come il main loop del firmware.
    void updateEngineParams() {
        const int modeP[2] = {MODE_A_PARAM, MODE_B_PARAM};
        const int posP[2] = {POS_A_PARAM, POS_B_PARAM};
        const int sizeP[2] = {SIZE_A_PARAM, SIZE_B_PARAM};
        const int pitchP[2] = {PITCH_A_PARAM, PITCH_B_PARAM};
        const int mixP[2] = {IO_MIX_A_PARAM, IO_MIX_B_PARAM};

        for (int d = 0; d < 2; d++) {
            auto ref = (Deck::Ref)d;
            auto& deck = core.deck(ref);

            const Mode mode = (Mode)(int)std::round(params[modeP[d]].getValue());
            if (deck.mode() != mode) {
                deck.set_mode(mode);
                core.infer_panner_mode();
            }

            deck.voxs().set_start(params[posP[d]].getValue());
            deck.voxs().set_size(params[sizeP[d]].getValue(), false);
            if (mode == Mode::Slice)
                deck.voxs().set_pitch(params[pitchP[d]].getValue());
            else
                deck.voxs().set_speed(params[pitchP[d]].getValue());
            deck.set_inout_mix(params[mixP[d]].getValue());
        }

        core.set_mix(params[CROSSFADE_PARAM].getValue());
    }

    /// Equivalente di CoreUI::read_cv(): va eseguito a ogni blocco.
    ///
    /// pitch_speed_mod_in() NON è opzionale anche senza cavo in V/oct: in Reel
    /// il moltiplicatore che imposta viene applicato a ogni aggiornamento
    /// dell'incremento del playhead. Senza questa chiamata, il primo movimento
    /// del pitch azzera l'incremento e la riproduzione si ferma.
    void readCV() {
        const int voctIn[2] = {VOCT_A_INPUT, VOCT_B_INPUT};

        for (int d = 0; d < 2; d++) {
            auto& deck = core.deck((Deck::Ref)d);
            const float semitones = inputs[voctIn[d]].getVoltage() * 12.f;
            speedMult[d] = speedMap.bipolar_pitch2speed(semitones);
            deck.voxs().pitch_speed_mod_in(speedMult[d]);
        }
    }

    void runEngineBlock(const dsp::Frame<2>* in, int inLen, dsp::Frame<4>* out) {
        float inL[kBlock] = {};
        float inR[kBlock] = {};
        float outL[kBlock] = {};
        float outR[kBlock] = {};

        for (int i = 0; i < inLen && i < kBlock; i++) {
            inL[i] = in[i].samples[0];
            inR[i] = in[i].samples[1];
        }

        updateEngineParams();
        readCV();

        // Clock: fronte di salita campionato a rate di blocco, come CoreUI::tick.
        const bool rising = clockHigh && !prevClockHigh;
        prevClockHigh = clockHigh;
        core.driver().tick(rising);

        // Gate in: latch accumulato per campione, consumato una volta per blocco.
        for (int d = 0; d < 2; d++) {
            if (gateLatched[d]) {
                gateLatched[d] = false;
                Event e = make_event();
                e.p3 = speedMult[d]; // come CoreUI::_trigger: la V/oct arriva nell'evento
                e.p3_on = true;
                core.deck((Deck::Ref)d).trigger(&e);
            }
        }

        core.prepare();

        float* inPtr[2] = {inL, inR};
        float* outPtr[2] = {outL, outR};
        core.process(inPtr, outPtr, kBlock);

        for (int i = 0; i < kBlock; i++) {
            float a = 0.f;
            float b = 0.f;
            core.mod(Deck::A).process(a);
            core.mod(Deck::B).process(b);

            out[i].samples[0] = outL[i];
            out[i].samples[1] = outR[i];
            out[i].samples[2] = a;
            out[i].samples[3] = b;
        }

        // read_reset_is_triggered() è un flag consume-once: va letto esattamente
        // una volta per blocco.
        for (int d = 0; d < 2; d++) {
            if (core.deck((Deck::Ref)d).voxs().read_reset_is_triggered()) {
                gateOutPulse[d].trigger(7e-3f); // 7 ms, come l'hardware
            }
        }
    }

    void process(const ProcessArgs& args) override {
        // --- controlli a rate di Rack ---
        const int playP[2] = {PLAY_A_PARAM, PLAY_B_PARAM};
        const int recP[2] = {REC_A_PARAM, REC_B_PARAM};
        const int gateIn[2] = {GATE_A_INPUT, GATE_B_INPUT};

        for (int d = 0; d < 2; d++) {
            auto ref = (Deck::Ref)d;
            auto& deck = core.deck(ref);

            // PLAY e REC sono latch che devono *rispecchiare* il motore, non
            // comandarlo alla cieca: Deck::toggle_play può non fare nulla
            // (buffer vuoto) o accodare l'avvio al prossimo key tick. Se il
            // parametro restasse indipendente, lo screen reader annuncerebbe
            // uno stato che non corrisponde all'audio.
            //
            // Quindi: una variazione del latch = richiesta dell'utente; poi il
            // latch viene riallineato alla verità del motore.
            const bool playLatch = params[playP[d]].getValue() > 0.5f;
            if (playLatch != playShadow[d]) {
                playShadow[d] = playLatch;
                deck.disarm();
                if (!deck.is_overdubbing()) {
                    core.driver().toggle_play(ref);
                }
            }
            // In coda conta già come "in riproduzione": è l'intenzione, ed
            // evita che il pulsante scatti indietro sotto le dita.
            const bool playTruth = deck.is_playing() || deck.is_play_queued();
            if (playTruth != playLatch) {
                params[playP[d]].setValue(playTruth ? 1.f : 0.f);
                playShadow[d] = playTruth;
            }

            const bool recLatch = params[recP[d]].getValue() > 0.5f;
            if (recLatch != recShadow[d]) {
                recShadow[d] = recLatch;
                core.set_source(Deck::Source::external, ref);
                deck.toggle_recording();
            }
            const bool recTruth = deck.is_recording() || deck.is_armed();
            if (recTruth != recLatch) {
                params[recP[d]].setValue(recTruth ? 1.f : 0.f);
                recShadow[d] = recTruth;
            }

            if (gateTrigger[d].process(inputs[gateIn[d]].getVoltage(), 0.1f, 1.f)
                == dsp::SchmittTrigger::Event::TRIGGERED) {
                gateLatched[d] = true;
            }
        }

        if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)
            == dsp::SchmittTrigger::Event::TRIGGERED) {
            clockHigh = true;
        }
        else if (inputs[CLOCK_INPUT].getVoltage() < 0.1f) {
            clockHigh = false;
        }

        // --- ingresso ---
        if (!inputBuffer.full()) {
            dsp::Frame<2> f;
            f.samples[0] = inputs[IN_L_INPUT].getVoltageSum() / 5.f;
            f.samples[1] = inputs[IN_R_INPUT].isConnected()
                               ? inputs[IN_R_INPUT].getVoltageSum() / 5.f
                               : f.samples[0];
            inputBuffer.push(f);
        }

        // --- un blocco di motore quando serve ---
        if (outputBuffer.empty()) {
            dsp::Frame<2> inFrames[kBlock] = {};
            int inLen = inputBuffer.size();
            int outLen = kBlock;
            inputSrc.setRates((int)args.sampleRate, (int)kEngineSR);
            inputSrc.process(inputBuffer.startData(), &inLen, inFrames, &outLen);
            inputBuffer.startIncr(inLen);

            dsp::Frame<4> engineOut[kBlock] = {};
            runEngineBlock(inFrames, outLen, engineOut);

            outputSrc.setRates((int)kEngineSR, (int)args.sampleRate);
            int srcInLen = kBlock;
            int srcOutLen = outputBuffer.capacity();
            outputSrc.process(engineOut, &srcInLen, outputBuffer.endData(), &srcOutLen);
            outputBuffer.endIncr(srcOutLen);
        }

        // --- uscita ---
        if (!outputBuffer.empty()) {
            dsp::Frame<4> f = outputBuffer.shift();
            outputs[OUT_L_OUTPUT].setVoltage(5.f * f.samples[0]);
            outputs[OUT_R_OUTPUT].setVoltage(5.f * f.samples[1]);
            outputs[MOD_A_OUTPUT].setVoltage(10.f * f.samples[2]);
            outputs[MOD_B_OUTPUT].setVoltage(10.f * f.samples[3]);
        }

        outputs[GATE_A_OUTPUT].setVoltage(gateOutPulse[0].process(args.sampleTime) ? 10.f : 0.f);
        outputs[GATE_B_OUTPUT].setVoltage(gateOutPulse[1].process(args.sampleTime) ? 10.f : 0.f);
        outputs[CLOCK_OUTPUT].setVoltage(clockOutPulse.process(args.sampleTime) ? 10.f : 0.f);
        lights[CLOCK_LIGHT].setBrightness(quarterPulse.process(args.sampleTime) ? 1.f : 0.f);

        // Piena se suona davvero, smorzata se l'avvio è solo accodato al
        // prossimo key tick: distingue a colpo d'occhio intenzione e realtà.
        for (int d = 0; d < 2; d++) {
            auto& deck = core.deck((Deck::Ref)d);
            lights[d == 0 ? PLAY_A_LIGHT : PLAY_B_LIGHT].setBrightness(
                deck.is_playing() ? 1.f : (deck.is_play_queued() ? 0.25f : 0.f));
            lights[d == 0 ? REC_A_LIGHT : REC_B_LIGHT].setBrightness(
                deck.is_recording() ? 1.f : (deck.is_armed() ? 0.25f : 0.f));
        }
    }
};

struct DoubleDeckWidget : ModuleWidget {
    DoubleDeckWidget(DoubleDeckModule* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/panels/DoubleDeck.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Layout provvisorio a griglia: il pannello definitivo arriva in fase 5.
        const float colA = 12.f;
        const float colB = 52.f;
        const float colG = 92.f;

        struct Row { float y; int pa; int pb; };
        const Row rows[] = {
            {24.f, DoubleDeckModule::POS_A_PARAM, DoubleDeckModule::POS_B_PARAM},
            {40.f, DoubleDeckModule::SIZE_A_PARAM, DoubleDeckModule::SIZE_B_PARAM},
            {56.f, DoubleDeckModule::PITCH_A_PARAM, DoubleDeckModule::PITCH_B_PARAM},
            {72.f, DoubleDeckModule::IO_MIX_A_PARAM, DoubleDeckModule::IO_MIX_B_PARAM},
        };
        for (const Row& r : rows) {
            addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(colA, r.y)), module, r.pa));
            addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(colB, r.y)), module, r.pb));
        }

        addParam(createParamCentered<CKSSThree>(mm2px(Vec(colA, 90.f)), module, DoubleDeckModule::MODE_A_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(colB, 90.f)), module, DoubleDeckModule::MODE_B_PARAM));

        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
            mm2px(Vec(colA, 104.f)), module, DoubleDeckModule::PLAY_A_PARAM, DoubleDeckModule::PLAY_A_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
            mm2px(Vec(colB, 104.f)), module, DoubleDeckModule::PLAY_B_PARAM, DoubleDeckModule::PLAY_B_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
            mm2px(Vec(colA, 114.f)), module, DoubleDeckModule::REC_A_PARAM, DoubleDeckModule::REC_A_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
            mm2px(Vec(colB, 114.f)), module, DoubleDeckModule::REC_B_PARAM, DoubleDeckModule::REC_B_LIGHT));

        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(colG, 24.f)), module, DoubleDeckModule::CROSSFADE_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colG, 44.f)), module, DoubleDeckModule::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colG, 56.f)), module, DoubleDeckModule::IN_R_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colG, 68.f)), module, DoubleDeckModule::GATE_A_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colG, 80.f)), module, DoubleDeckModule::GATE_B_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colG, 92.f)), module, DoubleDeckModule::CLOCK_INPUT));
        // Accanto al rispettivo knob PITCH (riga y=56), non sopra.
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colA + 16.f, 56.f)), module, DoubleDeckModule::VOCT_A_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colB + 16.f, 56.f)), module, DoubleDeckModule::VOCT_B_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colG + 16.f, 44.f)), module, DoubleDeckModule::OUT_L_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colG + 16.f, 56.f)), module, DoubleDeckModule::OUT_R_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colG + 16.f, 68.f)), module, DoubleDeckModule::GATE_A_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colG + 16.f, 80.f)), module, DoubleDeckModule::GATE_B_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colG + 16.f, 92.f)), module, DoubleDeckModule::MOD_A_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colG + 16.f, 104.f)), module, DoubleDeckModule::MOD_B_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colG + 16.f, 116.f)), module, DoubleDeckModule::CLOCK_OUTPUT));
        addChild(createLightCentered<MediumLight<WhiteLight>>(mm2px(Vec(colG, 104.f)), module, DoubleDeckModule::CLOCK_LIGHT));
    }
};

Model* modelDoubleDeck = createModel<DoubleDeckModule, DoubleDeckWidget>("DoubleDeck");
