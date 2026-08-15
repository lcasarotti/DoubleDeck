#include "plugin.hpp"

#include <osdialog.h>

#include <cstring>
#include <mutex>

#include "ParamMap.hpp"
#include "SampleLoader.hpp"
#include "buffer_pool.h"
#include "core/core.h"
#include "core/event.h"
#include "core/mode.h"
#include "core/tempo.h"
#include "ui/speed.map.h" // matematica pura: 2^(semitoni/12), riusata dal firmware

using namespace spotykach;
using namespace dd;

// Il motore gira sempre a 48 kHz in blocchi da 96 campioni (2 ms), esattamente
// come sull'hardware: le costanti DSP del firmware sono tarate su quel rate e
// non vengono toccate. L'adattamento al sample rate di Rack avviene con due
// SampleRateConverter, bypassati quando Rack gira già a 48 kHz.
static constexpr int kBlock = (int)spotykach::kEngineBlockSize;
static constexpr float kEngineSR = spotykach::kEngineSampleRate;
static constexpr float kBlockSeconds = kBlock / kEngineSR;

/// Lunghezze del buffer di loop offerte dal menu contestuale. 42 s è il valore
/// dell'hardware e costa ~36 MB per istanza.
static const std::vector<float> kBufferSeconds = {10.f, 20.f, 42.f};

/// Estensioni accettate dall'import. Solo WAV: è quello che legge e scrive
/// l'hardware, cue point compresi.
static const char kSampleFilters[] = "WAV:wav,WAV";

struct DoubleDeckModule : Module {
    BufferPool pool;
    Core core;

    /// Protegge motore e pool contro la riallocazione richiesta dal menu, che
    /// arriva dal thread UI. Il thread audio non aspetta mai: se non riesce a
    /// prendere il lock esce in silenzio per quel campione.
    std::mutex engineMutex;

    dsp::SampleRateConverter<2> inputSrc;
    dsp::SampleRateConverter<4> outputSrc;
    dsp::DoubleRingBuffer<dsp::Frame<2>, 256> inputBuffer;
    dsp::DoubleRingBuffer<dsp::Frame<4>, 256> outputBuffer;

    /// Ultimo valore spinto nel motore, per parametro. NaN significa "da
    /// rispingere": il confronto con NaN è sempre falso, quindi il primo giro
    /// inizializza tutto il motore da solo.
    float pushed[PARAMS_LEN];
    /// Ultimo stato noto dei latch, per distinguere l'azione dell'utente dal
    /// riallineamento che facciamo noi.
    bool latchShadow[PARAMS_LEN];

    dsp::BooleanTrigger buttonTrigger[PARAMS_LEN];

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

    // Stato solo di visualizzazione, aggiornato a rate di blocco.
    int gateInLightCount[2] = {0, 0};
    float modValue[2] = {0.f, 0.f};
    /// Blocchi per cui rispecchiare il tempo del motore nel parametro dopo un
    /// tap: il clock lo recepisce al tick successivo, non subito.
    int tempoMirrorCount = 0;

    // Opzioni del menu contestuale.
    float bufferSeconds = 42.f;
    bool sliceMono[2] = {false, false};

    // --- campione importato, per deck ---
    /// Path del file, non l'audio: 42 s stereo sono 16 MB per deck, che in un
    /// `.vcv` non ci stanno.
    std::string samplePath[2];
    /// Riassunto dell'ultimo import, mostrato nel menu (o l'errore, se il file
    /// non si è aperto — tipico al caricamento di una patch che punta a un file
    /// spostato).
    std::string sampleInfo[2];
    /// Ultima cartella visitata, per riaprire il dialogo dove si era rimasti.
    std::string sampleDir;

    DoubleDeckModule() : pool(42.f) {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configureAll(this);
        initEngine();
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        std::lock_guard<std::mutex> lock(engineMutex);
        bufferSeconds = 42.f;
        sliceMono[0] = sliceMono[1] = false;
        for (int d = 0; d < 2; d++) {
            samplePath[d].clear();
            sampleInfo[d].clear();
        }
        pool.allocate(bufferSeconds);
        initEngine();
    }

    /// Init del motore. Il chiamante tiene `engineMutex` quando il modulo è già
    /// in esecuzione.
    void initEngine() {
        speedMap.init();

        for (int i = 0; i < PARAMS_LEN; i++) {
            pushed[i] = NAN; // tutto da rispingere
            latchShadow[i] = false;
        }

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
    }

    void setBufferSeconds(float seconds) {
        {
            std::lock_guard<std::mutex> lock(engineMutex);
            bufferSeconds = seconds;
            pool.allocate(seconds);
            initEngine();
        }
        // Fuori dal lock, che non è ricorsivo: il pool nuovo è vuoto, quindi i
        // campioni importati vanno riletti dai loro file.
        for (int d = 0; d < 2; d++) {
            if (!samplePath[d].empty()) reloadSample(d);
        }
    }

    // --- Import di campioni ------------------------------------------------
    //
    // È la SD card dell'hardware: `Buffer::raw()` e `Buffer::set_rec_size()`
    // sono la stessa via che usa src/memory/storage.cpp per caricare un tape.
    // Tutto avviene sul thread UI; sotto il lock resta la sola copia, mentre
    // lettura e conversione del file — le parti lente — stanno fuori.

    /// Carica `path` nel deck `d`. Ritorna false con il motivo in `error`.
    /// Il chiamante NON deve tenere `engineMutex`.
    bool loadSample(int d, const std::string& path, std::string& error) {
        SampleData data;
        if (!loadSampleFile(path, pool.sourceBufferSize(), data, error)) {
            // Il path non lo tocchiamo: se il deck aveva già un campione buono,
            // un tentativo andato male non deve cancellarlo dalla patch. Nel
            // caso opposto — patch che punta a un file spostato — il path è già
            // quello, e resta.
            sampleInfo[d] = system::getFilename(path) + " — " + error;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(engineMutex);
            auto& deck = core.deck((Deck::Ref)d);

            // Niente lettura né scrittura mentre il contenuto cambia sotto.
            // Nota: in Slice un rec appena armato resta accodato al prossimo
            // quarto anche dopo `disarm()` — è il comportamento del firmware
            // (Deck::_clock_recording), non una svista di qui.
            deck.disarm();
            deck.stop();

            auto& buffer = deck.buffer();
            buffer.clear(); // azzera anche la coda oltre il campione
            std::memcpy(buffer.raw(), data.frames.data(),
                        data.frames.size() * sizeof(Buffer::Frame));

            // I cue point si scrivono nell'array del generatore, come fa Card
            // in fase di lettura: `add_cue()` prenderebbe la posizione della
            // testina, non quella del file.
            auto& voxs = deck.voxs();
            voxs.clear_cue();
            for (size_t i = 0; i < data.cuePoints.size(); i++) {
                voxs.cue_points()[i] = data.cuePoints[i];
            }
            *voxs.cue_count() = (uint8_t)data.cuePoints.size();

            buffer.set_rec_size(data.frames.size());
            if (deck.mode() == Mode::Slice) deck.make_grid();

            // Con dei cue point la size non viene più elevata al quadrato
            // (Generator::set_size), e in ogni caso il riferimento è cambiato:
            // i due parametri vanno rispinti perché il motore li rilegga.
            repush(SIZE_A_PARAM + d);
            repush(POS_A_PARAM + d);
        }

        samplePath[d] = path;
        sampleInfo[d] = string::f("%s — %.2f s%s", system::getFilename(path).c_str(),
                                  (float)data.frames.size() / kEngineSR,
                                  data.truncated ? ", truncated" : "");
        if (!data.cuePoints.empty()) {
            sampleInfo[d] += string::f(", %d cue points", (int)data.cuePoints.size());
        }
        INFO("Double Deck: deck %c loaded %s (%d Hz, %d ch) -> %d frames, %d cue points%s",
             d == 0 ? 'A' : 'B', path.c_str(), data.sourceRate, data.sourceChannels,
             (int)data.frames.size(), (int)data.cuePoints.size(),
             data.truncated ? ", truncated" : "");
        return true;
    }

    /// Ricarica il file già scelto: al caricamento della patch e dopo un cambio
    /// di lunghezza del buffer. Un file sparito non è un errore da dialogo — la
    /// patch può venire da un'altra macchina — quindi resta scritto nel menu.
    void reloadSample(int d) {
        const std::string path = samplePath[d];
        std::string error;
        if (!loadSample(d, path, error)) {
            WARN("Double Deck: deck %c could not load %s: %s", d == 0 ? 'A' : 'B',
                 path.c_str(), error.c_str());
        }
    }

    /// Voce di menu: sceglie il file e lo carica.
    void loadSampleDialog(int d) {
        osdialog_filters* filters = osdialog_filters_parse(kSampleFilters);
        DEFER({ osdialog_filters_free(filters); });

        char* pathC = osdialog_file(OSDIALOG_OPEN, sampleDir.empty() ? NULL : sampleDir.c_str(),
                                    NULL, filters);
        if (!pathC) return; // annullato
        const std::string path = pathC;
        std::free(pathC);
        sampleDir = system::getDirectory(path);

        std::string error;
        if (!loadSample(d, path, error)) {
            // Qui il file l'ha appena scelto l'utente: fallire in silenzio
            // sarebbe incomprensibile.
            osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK,
                             string::f("Could not load %s:\n%s",
                                       system::getFilename(path).c_str(), error.c_str())
                                 .c_str());
        }
    }

    /// Svuota il buffer del deck: è anche il modo di togliere un campione
    /// importato senza doverci registrare sopra.
    void clearDeck(int d) {
        {
            std::lock_guard<std::mutex> lock(engineMutex);
            auto& deck = core.deck((Deck::Ref)d);
            deck.disarm();
            deck.stop();
            deck.buffer().clear();
            deck.voxs().clear_cue();
            repush(SIZE_A_PARAM + d);
            repush(POS_A_PARAM + d);
        }
        samplePath[d].clear();
        sampleInfo[d].clear();
    }

    // --- Parametri -------------------------------------------------------
    //
    // Ciò che sull'hardware sta in CoreUI::process: gira una volta per blocco,
    // cioè a 500 Hz come il main loop del firmware. Niente pickup e niente
    // layer — ogni funzione ha il suo parametro.

    /// Vero se il parametro è cambiato da quando l'abbiamo spinto nel motore.
    bool moved(int id) {
        const float v = params[id].getValue();
        if (v == pushed[id]) return false;
        pushed[id] = v;
        return true;
    }

    /// Marca un parametro perché venga rispinto anche se non si è mosso: serve
    /// quando cambia il contesto che ne determina l'effetto (modo del deck,
    /// quantizzazione, modo del Grit).
    void repush(int id) { pushed[id] = NAN; }

    /// Scrive nel parametro senza che il giro successivo lo riconosca come
    /// mossa dell'utente.
    void syncParam(int id, float value) {
        params[id].setValue(value);
        pushed[id] = value;
    }

    /// Un latch è una **richiesta**, non un comando: il motore non obbedisce
    /// sempre (`toggle_play` non fa nulla a buffer vuoto, e in Slice accoda
    /// l'avvio al prossimo key tick). Dopo la richiesta il latch va riallineato
    /// allo stato reale, altrimenti lo screen reader annuncia uno stato che
    /// l'audio non ha.
    template <typename Request, typename Truth>
    void syncLatch(int id, Request&& request, Truth&& truth) {
        const bool wanted = params[id].getValue() > .5f;
        if (wanted != latchShadow[id]) {
            latchShadow[id] = wanted;
            request(wanted);
        }
        const bool real = truth();
        if (real != wanted) {
            params[id].setValue(real ? 1.f : 0.f);
            latchShadow[id] = real;
        }
    }

    /// Vero una sola volta per pressione.
    bool pressed(int id) {
        return buttonTrigger[id].process(params[id].getValue() > .5f);
    }

    void updateDeckParams(const int d) {
        const auto ref = (Deck::Ref)d;
        auto& deck = core.deck(ref);
        auto& voxs = deck.voxs();
        auto& fx = deck.fx();
        auto& mod = core.mod(ref);

        // --- modo: cambia il significato di metà dei parametri, quindi va per
        // primo. Come nel firmware, dopo il cambio la size va rispinta.
        if (moved(MODE_A_PARAM + d)) {
            const auto mode = (Mode)(int)std::round(params[MODE_A_PARAM + d].getValue());
            if (deck.mode() != mode) {
                deck.set_mode(mode);
                core.infer_panner_mode();
                repush(SIZE_A_PARAM + d);
                // Vox::set_win_size ignora la chiamata fuori da Drift: senza
                // rispinta, entrando in Drift la finestra resterebbe quella
                // vecchia invece di quella che il parametro dichiara.
                repush(WINDOW_A_PARAM + d);
            }
        }

        // --- trasporto ---------------------------------------------------
        syncLatch(
            PLAY_A_PARAM + d,
            [&](bool) {
                deck.disarm();
                if (!deck.is_overdubbing()) core.driver().toggle_play(ref);
            },
            // "In coda" conta già come in riproduzione: è l'intenzione, ed
            // evita che il pulsante scatti indietro sotto le dita.
            [&] { return deck.is_playing() || deck.is_play_queued(); });

        syncLatch(
            REC_A_PARAM + d,
            [&](bool) { deck.toggle_recording(); },
            [&] { return deck.is_recording() || deck.is_armed(); });

        if (moved(REC_SOURCE_A_PARAM + d)) {
            // A differenza dell'hardware, che la fissava all'istante del rec,
            // qui la sorgente è uno switch vero e vale anche a registrazione
            // in corso.
            const bool internal = params[REC_SOURCE_A_PARAM + d].getValue() > .5f;
            core.set_source(internal ? Deck::Source::internal : Deck::Source::external, ref);
        }

        // Cambiare direzione mentre il deck suona inverte senza fermare: è la
        // semantica del pad Rev dell'hardware, qui gratis.
        if (moved(REVERSE_A_PARAM + d)) {
            deck.set_reverse(params[REVERSE_A_PARAM + d].getValue() > .5f);
        }

        if (pressed(TRIGGER_A_PARAM + d)) {
            Event e = make_event();
            deck.trigger(&e);
            gateInLightCount[d] = kGateLightBlocks;
        }

        // --- lettura del loop ---------------------------------------------
        if (moved(POS_A_PARAM + d)) voxs.set_start(params[POS_A_PARAM + d].getValue());

        if (moved(START_OFFSET_A_PARAM + d)) {
            // set_start_offset_interval riconverte in 0…8 con un round.
            voxs.set_start_offset_interval(params[START_OFFSET_A_PARAM + d].getValue() / 8.f);
        }

        if (moved(SIZE_A_PARAM + d)) {
            // Il secondo argomento (`alt`) sceglie fra snap e free sui cue
            // point, ma generator.cpp forza CueSizeMode::ignore, quindi oggi è
            // inerte: passiamo false e non gli dedichiamo un parametro.
            voxs.set_size(params[SIZE_A_PARAM + d].getValue(), false);
        }

        // La quantizzazione non è un valore a sé: cambia ciò che spingiamo come
        // pitch, quindi obbliga a rispingerlo.
        if (moved(PITCH_QUANT_A_PARAM + d)) repush(PITCH_A_PARAM + d);
        if (moved(PITCH_A_PARAM + d)) {
            auto pitch = params[PITCH_A_PARAM + d].getValue();
            if (params[PITCH_QUANT_A_PARAM + d].getValue() > .5f) pitch = snappedSpeed(pitch);
            // In Slice il knob è intonazione (time-stretch); altrove è velocità
            // di lettura, che intona e accorcia insieme.
            if (deck.mode() == Mode::Slice) voxs.set_pitch(pitch);
            else voxs.set_speed(pitch);
        }

        if (moved(IO_MIX_A_PARAM + d)) deck.set_inout_mix(params[IO_MIX_A_PARAM + d].getValue());
        if (moved(FEEDBACK_A_PARAM + d)) deck.set_feedback(params[FEEDBACK_A_PARAM + d].getValue());

        if (moved(ENV_SHAPE_A_PARAM + d)) voxs.set_shape(params[ENV_SHAPE_A_PARAM + d].getValue());
        if (moved(ENV_SIZE_A_PARAM + d)) voxs.set_env_size(params[ENV_SIZE_A_PARAM + d].getValue());
        if (moved(WINDOW_A_PARAM + d)) voxs.set_win_size(params[WINDOW_A_PARAM + d].getValue());

        // --- destinazione del CV size/pos ---------------------------------
        if (moved(CV_DEST_A_PARAM + d)) {
            const int dest = (int)std::round(params[CV_DEST_A_PARAM + d].getValue());
            voxs.set_size_mod_on(dest <= 1);
            voxs.set_start_mod_on(dest >= 1);
        }

        // --- modulatore -----------------------------------------------------
        if (moved(MOD_TYPE_A_PARAM + d)) {
            mod.set_type(params[MOD_TYPE_A_PARAM + d].getValue() > .5f ? Modulator::Type::LFO
                                                                      : Modulator::Type::Follow);
        }
        if (moved(LFO_SHAPE_A_PARAM + d)) {
            mod.set_lfo_type((LFO::Type)(int)std::round(params[LFO_SHAPE_A_PARAM + d].getValue()));
        }
        if (moved(MOD_SYNC_A_PARAM + d)) repush(MOD_SPEED_A_PARAM + d);
        if (moved(MOD_SPEED_A_PARAM + d)) {
            mod.set_speed_norm(params[MOD_SPEED_A_PARAM + d].getValue(),
                               params[MOD_SYNC_A_PARAM + d].getValue() > .5f);
        }
        if (moved(MOD_AMOUNT_A_PARAM + d)) mod.set_amp_norm(params[MOD_AMOUNT_A_PARAM + d].getValue());

        // --- Grit ------------------------------------------------------------
        syncLatch(
            GRIT_ON_A_PARAM + d,
            [&](bool on) { fx.set_grit_on(on); },
            [&] { return fx.is_grit_on(); });

        if (moved(GRIT_MODE_A_PARAM + d)) {
            const auto mode = params[GRIT_MODE_A_PARAM + d].getValue() > .5f ? Fx::GritMode::Reduce
                                                                            : Fx::GritMode::Drive;
            if (fx.grit_mode() != mode) {
                fx.switch_grit_mode();
                // Intensità e mix sono per modo: sull'hardware il firmware
                // riportava i knob al valore del nuovo modo, qui sono i nostri
                // parametri a comandare, quindi li rispingiamo.
                repush(GRIT_INTENS_A_PARAM + d);
                repush(GRIT_MIX_A_PARAM + d);
            }
        }
        if (moved(GRIT_INTENS_A_PARAM + d)) fx.set_grit_intensity(params[GRIT_INTENS_A_PARAM + d].getValue());
        if (moved(GRIT_MIX_A_PARAM + d)) fx.set_grit_mix(params[GRIT_MIX_A_PARAM + d].getValue());

        // --- Flux -------------------------------------------------------------
        syncLatch(
            FLUX_ON_A_PARAM + d,
            [&](bool on) { fx.set_flux_on(on); },
            [&] { return fx.is_flux_on(); });

        if (moved(FLUX_INTENS_A_PARAM + d)) fx.set_flux_intensity(params[FLUX_INTENS_A_PARAM + d].getValue());
        if (moved(FLUX_FB_A_PARAM + d)) fx.set_flux_fb(params[FLUX_FB_A_PARAM + d].getValue());
        if (moved(FLUX_MIX_A_PARAM + d)) fx.set_flux_mix(params[FLUX_MIX_A_PARAM + d].getValue());

        // --- sequencer -----------------------------------------------------------
        auto& track = deck.track();
        syncLatch(
            SEQ_ARM_A_PARAM + d,
            [&](bool on) {
                if (on) track.arm(!core.driver().is_key_sub_quarter());
                else track.disarm();
            },
            // Solo is_armed(): disarmando a sequenza avviata la registrazione
            // prosegue fino a fine giro (Track::_is_auto_cutting), e prendere
            // per vero is_recording farebbe riscattare il latch.
            [&] { return track.is_armed(); });

        if (pressed(SEQ_CLEAR_A_PARAM + d)) deck.clear_sequence();

        // --- tempo dal loop --------------------------------------------------------
        // Il knob dichiara in quanti quarti sta il loop; il pulsante applica.
        // Sull'hardware i due gesti erano lo stesso (knob sotto TAP-HOLD).
        if (pressed(FIT_TEMPO_A_PARAM + d) && !deck.is_empty()) {
            const float quarters = params[SIZE_QUARTERS_A_PARAM + d].getValue();
            const float bpm = deck.tempo_to_fit((quarters - 1.f) / 15.f);
            core.driver().set_tempo_norm(Tempo::abs_to_norm(bpm));
            // Un loop molto corto o molto lungo chiede un tempo fuori scala: il
            // driver lo satura a 20…250, e il parametro deve dire la stessa cosa.
            syncParam(TEMPO_PARAM, clamp(bpm, 20.f, 250.f));
        }

        deck.set_slice_mono(sliceMono[d]);
    }

    void updateGlobalParams() {
        auto& driver = core.driver();

        if (moved(CROSSFADE_PARAM)) core.set_mix(params[CROSSFADE_PARAM].getValue());
        if (moved(CLICK_MIX_PARAM)) core.set_click_mix(params[CLICK_MIX_PARAM].getValue());
        if (moved(PAN_SPEED_PARAM)) core.panner().set_speed(params[PAN_SPEED_PARAM].getValue());
        if (moved(PAN_RANGE_PARAM)) core.panner().set_range(params[PAN_RANGE_PARAM].getValue());

        if (moved(ROUTE_PARAM)) {
            // Route parte da 1 (DoubleMono), il parametro da 0.
            core.set_route((Route)((int)std::round(params[ROUTE_PARAM].getValue()) + 1));
        }

        if (moved(TEMPO_PARAM)) {
            driver.set_tempo_norm(Tempo::abs_to_norm(params[TEMPO_PARAM].getValue()));
        }
        if (moved(KEY_INTERVAL_PARAM)) {
            driver.set_key_tick_interval_norm(params[KEY_INTERVAL_PARAM].getValue() / 16.f);
        }

        if (moved(CLOCK_SOURCE_PARAM)) {
            // toggle_source cicla internal → ts4 → midi: il MIDI del firmware è
            // fuori scope, quindi ci arriviamo girando finché serve.
            const auto target = params[CLOCK_SOURCE_PARAM].getValue() > .5f ? Driver::Source::ts4
                                                                           : Driver::Source::internal;
            for (int i = 0; i < 3 && driver.source() != target; i++) driver.toggle_source();
        }

        if (pressed(TAP_PARAM) && !driver.is_external_sync()) {
            driver.tap_tempo();
            tempoMirrorCount = kTempoMirrorBlocks;
        }
        if (pressed(CLOCK_RESET_PARAM)) driver.reset();

        // Il tempo lo mostriamo com'è davvero quando non lo decide il knob:
        // sotto clock esterno lo detta il clock, e dopo un tap il valore arriva
        // al tick successivo.
        if (tempoMirrorCount > 0) tempoMirrorCount--;
        if (driver.is_external_sync() || tempoMirrorCount > 0) {
            const float bpm = driver.tempo();
            if (bpm > 0.f && std::abs(bpm - params[TEMPO_PARAM].getValue()) > .05f) {
                syncParam(TEMPO_PARAM, clamp(bpm, 20.f, 250.f));
            }
        }
    }

    /// Equivalente di CoreUI::read_cv(): va eseguito a ogni blocco.
    ///
    /// pitch_speed_mod_in() NON è opzionale anche senza cavo in V/oct: in Reel
    /// il moltiplicatore che imposta viene applicato a ogni aggiornamento
    /// dell'incremento del playhead. Senza questa chiamata, il primo movimento
    /// del pitch azzera l'incremento e la riproduzione si ferma.
    void readCV() {
        for (int d = 0; d < 2; d++) {
            auto& deck = core.deck((Deck::Ref)d);

            const float semitones = inputs[VOCT_A_INPUT + d].getVoltage() * 12.f;
            speedMult[d] = speedMap.bipolar_pitch2speed(semitones);
            deck.voxs().pitch_speed_mod_in(speedMult[d]);

            deck.inout_mix_mod_in(inputs[CV_MIX_A_INPUT + d].getVoltage() / 5.f);

            // Il firmware arrotonda al millesimo per non far ballare la
            // dimensione del grano sul rumore dell'ADC; qui non c'è rumore, ma
            // teniamo la stessa quantizzazione per non cambiare il carattere.
            float sizePos = inputs[CV_SIZE_POS_A_INPUT + d].getVoltage() / 5.f;
            sizePos = std::round(sizePos * 1000.f) / 1000.f;
            deck.voxs().set_start_mod(sizePos);
            deck.voxs().set_size_mod(sizePos);
        }

        core.mix_mod_in(inputs[CV_CROSSFADE_INPUT].getVoltage() / 5.f);
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

        updateDeckParams(0);
        updateDeckParams(1);
        updateGlobalParams();
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
                gateInLightCount[d] = kGateLightBlocks;
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
        modValue[0] = out[kBlock - 1].samples[2];
        modValue[1] = out[kBlock - 1].samples[3];

        // read_reset_is_triggered() è un flag consume-once: va letto esattamente
        // una volta per blocco.
        for (int d = 0; d < 2; d++) {
            if (core.deck((Deck::Ref)d).voxs().read_reset_is_triggered()) {
                gateOutPulse[d].trigger(7e-3f); // 7 ms, come l'hardware
            }
        }

        updateLights();
    }

    /// A rate di blocco (500 Hz): le spie non hanno bisogno di più.
    void updateLights() {
        for (int d = 0; d < 2; d++) {
            auto& deck = core.deck((Deck::Ref)d);
            auto& fx = deck.fx();

            // Piena se suona davvero, smorzata se l'avvio è solo accodato al
            // prossimo key tick: distingue a colpo d'occhio intenzione e realtà.
            lights[PLAY_A_LIGHT + d].setBrightness(
                deck.is_playing() ? 1.f : (deck.is_play_queued() ? .25f : 0.f));
            lights[REC_A_LIGHT + d].setBrightness(
                deck.is_recording() ? 1.f : (deck.is_armed() ? .25f : 0.f));
            lights[SEQ_A_LIGHT + d].setBrightness(
                deck.track().is_recording() ? 1.f : (deck.track().is_armed() ? .25f : 0.f));
            lights[GRIT_A_LIGHT + d].setBrightness(fx.is_grit_on() ? 1.f : 0.f);
            lights[FLUX_A_LIGHT + d].setBrightness(fx.is_flux_on() ? 1.f : 0.f);

            if (gateInLightCount[d] > 0) gateInLightCount[d]--;
            lights[GATE_IN_A_LIGHT + d].setBrightness(gateInLightCount[d] > 0 ? 1.f : 0.f);

            // Luminosità = valore corrente del modulatore: il feedback più
            // informativo a costo quasi zero, viste le ring a 32 LED assenti.
            lights[MOD_A_LIGHT + d].setBrightness(clamp(modValue[d], 0.f, 1.f));
        }

        lights[CLOCK_LIGHT].setBrightness(quarterPulse.process(kBlockSeconds) ? 1.f : 0.f);
    }

    void process(const ProcessArgs& args) override {
        // Il thread audio non aspetta mai la riallocazione del pool.
        std::unique_lock<std::mutex> lock(engineMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            for (int i = 0; i < OUTPUTS_LEN; i++) outputs[i].setVoltage(0.f);
            return;
        }

        // --- controlli a rate di Rack ---
        for (int d = 0; d < 2; d++) {
            if (gateTrigger[d].process(inputs[GATE_A_INPUT + d].getVoltage(), 0.1f, 1.f)
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

        // Gate e clock out non passano dal SRC: sono impulsi, e a rate di Rack
        // restano sample-accurate.
        for (int d = 0; d < 2; d++) {
            outputs[GATE_A_OUTPUT + d].setVoltage(
                gateOutPulse[d].process(args.sampleTime) ? 10.f : 0.f);
        }
        outputs[CLOCK_OUTPUT].setVoltage(clockOutPulse.process(args.sampleTime) ? 10.f : 0.f);
    }

    // --- Stato leggibile -------------------------------------------------
    //
    // Senza gli anelli a 32 LED dell'hardware, lo stato a parole è il canale
    // più informativo che abbiamo — e in MetaRack è anche il più accessibile.

    /// Lunghezza corrente della finestra di lettura, in secondi. 0 se il deck
    /// è vuoto.
    float sizeSeconds(int d) {
        auto& deck = core.deck((Deck::Ref)d);
        if (deck.is_empty()) return 0.f;
        return deck.voxs().norm_size() * deck.buffer().rec_size() / kEngineSR;
    }

    std::string deckStatus(int d) {
        auto& deck = core.deck((Deck::Ref)d);
        std::string s = d == 0 ? "Deck A: " : "Deck B: ";
        if (deck.is_empty()) {
            s += deck.is_armed() ? "armed, empty" : "empty";
            return s;
        }
        if (deck.is_recording()) s += deck.is_playing() ? "overdubbing" : "recording";
        else if (deck.is_playing()) s += deck.is_reverse() ? "playing reverse" : "playing";
        else if (deck.is_play_queued()) s += "starting at next key tick";
        else s += "stopped";
        s += string::f(", loop %.2f s", deck.buffer().rec_size() / kEngineSR);
        if (!deck.track().is_empty()) s += ", sequence recorded";
        if (!sampleInfo[d].empty()) s += ", sample " + sampleInfo[d];
        return s;
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "bufferSeconds", json_real(bufferSeconds));
        json_object_set_new(root, "sliceMonoA", json_boolean(sliceMono[0]));
        json_object_set_new(root, "sliceMonoB", json_boolean(sliceMono[1]));
        // Del campione si salva il path, non l'audio.
        for (int d = 0; d < 2; d++) {
            if (samplePath[d].empty()) continue;
            json_object_set_new(root, d == 0 ? "samplePathA" : "samplePathB",
                                json_string(samplePath[d].c_str()));
        }
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "sliceMonoA")) sliceMono[0] = json_boolean_value(j);
        if (json_t* j = json_object_get(root, "sliceMonoB")) sliceMono[1] = json_boolean_value(j);

        // I path si leggono prima della lunghezza del buffer: se questa cambia,
        // è la riallocazione stessa a rileggere i file, e non li carichiamo due
        // volte.
        for (int d = 0; d < 2; d++) {
            json_t* j = json_object_get(root, d == 0 ? "samplePathA" : "samplePathB");
            samplePath[d] = j ? json_string_value(j) : "";
        }

        bool reallocated = false;
        if (json_t* j = json_object_get(root, "bufferSeconds")) {
            const float s = json_number_value(j);
            if (s > 0.f && s != bufferSeconds) {
                setBufferSeconds(s);
                reallocated = true;
            }
        }

        if (!reallocated) {
            for (int d = 0; d < 2; d++) {
                if (!samplePath[d].empty()) reloadSample(d);
            }
        }
    }

    /// Durata della spia di gate in, in blocchi da 2 ms.
    static constexpr int kGateLightBlocks = 25; // 50 ms
    /// Per quanti blocchi rispecchiare il tempo del motore dopo un tap.
    static constexpr int kTempoMirrorBlocks = 8;
};

float dd::SizeQuantity::seconds()
{
    if (auto* m = dynamic_cast<DoubleDeckModule*>(module)) return m->sizeSeconds(deck);
    return 0.f;
}

std::string dd::SizeQuantity::getDisplayValueString()
{
    const float sec = seconds();
    if (sec > 0.f) return string::f("%.2f", sec);
    return ParamQuantity::getDisplayValueString();
}

std::string dd::SizeQuantity::getUnit()
{
    return seconds() > 0.f ? " s" : ParamQuantity::getUnit();
}

// --- Pannello -------------------------------------------------------------
//
// Layout provvisorio ma completo: ogni parametro ha il suo widget, perché uno
// senza widget non è raggiungibile né col mouse né dalla vista PARAM. Il
// pannello definitivo arriva in fase 5.

/// Testo disegnato dal widget: nanosvg non rende gli elementi <text>, quindi le
/// etichette non possono stare nel pannello SVG.
struct PanelLabel : Widget {
    std::string text;
    float fontSize = 6.8f;
    NVGcolor color = nvgRGB(0xc8, 0xc8, 0xd0);

    PanelLabel(Vec center, const std::string& t) : text(t) {
        box.size = Vec(60.f, 10.f);
        box.pos = center.minus(box.size.div(2));
    }

    void draw(const DrawArgs& args) override {
        std::shared_ptr<window::Font> font
            = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (!font) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, fontSize);
        nvgFillColor(args.vg, color);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x / 2, box.size.y / 2, text.c_str(), NULL);
    }
};

namespace {

enum WType {
    KNOB,      ///< potenziometro continuo
    SNAP,      ///< potenziometro a scatti (per gli switch a più di tre posizioni)
    SW2,
    SW3,
    BUTTON,    ///< momentaneo
    LATCH,     ///< a scatto, con spia bianca
    LATCH_G,   ///< a scatto, spia verde
    LATCH_R,   ///< a scatto, spia rossa
    IN_PORT,
    OUT_PORT,
    LIGHT      ///< sola spia
};

struct Cell {
    float col;
    float row;
    WType type;
    int id;         ///< param / input / output / light, variante A
    int light;      ///< spia del LATCH, variante A; -1 se assente
    const char* label;
};

// Griglia: colonne larghe 18 mm, righe alte 13 mm.
constexpr float kColStep = 18.f;
constexpr float kRowStep = 13.f;
constexpr float kRow0 = 24.f;
constexpr float kDeckColA = 13.f;
constexpr float kDeckColB = 107.f;
constexpr float kGlobalCol = 201.f;

/// Un deck: 34 parametri e 6 porte in otto righe da cinque.
const Cell kDeckCells[] = {
    {0, 0, LATCH_G, PLAY_A_PARAM, PLAY_A_LIGHT, "PLAY"},
    {1, 0, LATCH_R, REC_A_PARAM, REC_A_LIGHT, "REC"},
    {2, 0, SW2, REC_SOURCE_A_PARAM, -1, "SOURCE"},
    {3, 0, SW2, REVERSE_A_PARAM, -1, "REV"},
    {4, 0, SW3, MODE_A_PARAM, -1, "MODE"},

    {0, 1, KNOB, POS_A_PARAM, -1, "POS"},
    {1, 1, KNOB, SIZE_A_PARAM, -1, "SIZE"},
    {2, 1, KNOB, PITCH_A_PARAM, -1, "PITCH"},
    {3, 1, KNOB, IO_MIX_A_PARAM, -1, "I/O MIX"},
    {4, 1, KNOB, FEEDBACK_A_PARAM, -1, "FEEDBK"},

    {0, 2, SNAP, START_OFFSET_A_PARAM, -1, "OFFSET"},
    {1, 2, KNOB, ENV_SHAPE_A_PARAM, -1, "ENV"},
    {2, 2, KNOB, ENV_SIZE_A_PARAM, -1, "ENV SZ"},
    {3, 2, KNOB, WINDOW_A_PARAM, -1, "WINDOW"},
    {4, 2, SW2, PITCH_QUANT_A_PARAM, -1, "QUANT"},

    {0, 3, SW2, MOD_TYPE_A_PARAM, -1, "MOD"},
    {1, 3, SNAP, LFO_SHAPE_A_PARAM, -1, "SHAPE"},
    {2, 3, SW2, MOD_SYNC_A_PARAM, -1, "SYNC"},
    {3, 3, KNOB, MOD_SPEED_A_PARAM, -1, "CYCLE"},
    {4, 3, KNOB, MOD_AMOUNT_A_PARAM, -1, "GLOW"},

    {0, 4, LATCH, GRIT_ON_A_PARAM, GRIT_A_LIGHT, "GRIT"},
    {1, 4, SW2, GRIT_MODE_A_PARAM, -1, "G MODE"},
    {2, 4, KNOB, GRIT_INTENS_A_PARAM, -1, "G INT"},
    {3, 4, KNOB, GRIT_MIX_A_PARAM, -1, "G MIX"},
    {4, 4, SW3, CV_DEST_A_PARAM, -1, "CV DST"},

    {0, 5, LATCH, FLUX_ON_A_PARAM, FLUX_A_LIGHT, "FLUX"},
    {1, 5, KNOB, FLUX_INTENS_A_PARAM, -1, "F TIME"},
    {2, 5, KNOB, FLUX_FB_A_PARAM, -1, "F FB"},
    {3, 5, KNOB, FLUX_MIX_A_PARAM, -1, "F MIX"},
    {4, 5, BUTTON, TRIGGER_A_PARAM, -1, "TRIG"},

    {0, 6, LATCH, SEQ_ARM_A_PARAM, SEQ_A_LIGHT, "SEQ"},
    {1, 6, BUTTON, SEQ_CLEAR_A_PARAM, -1, "CLEAR"},
    {2, 6, SNAP, SIZE_QUARTERS_A_PARAM, -1, "QUARTS"},
    {3, 6, BUTTON, FIT_TEMPO_A_PARAM, -1, "FIT"},
    {4, 6, OUT_PORT, GATE_A_OUTPUT, -1, "GATE"},

    {0, 7, IN_PORT, CV_SIZE_POS_A_INPUT, -1, "SIZE/POS"},
    {1, 7, IN_PORT, CV_MIX_A_INPUT, -1, "MIX CV"},
    {2, 7, IN_PORT, VOCT_A_INPUT, -1, "V/OCT"},
    {3, 7, IN_PORT, GATE_A_INPUT, -1, "GATE IN"},
    {4, 7, OUT_PORT, MOD_A_OUTPUT, -1, "MOD"},

    {3.42f, 6.62f, LIGHT, GATE_IN_A_LIGHT, -1, ""},
    {4.42f, 7.62f, LIGHT, MOD_A_LIGHT, -1, ""},
};

const Cell kGlobalCells[] = {
    {0, 0, KNOB, TEMPO_PARAM, -1, "TEMPO"},
    {1, 0, BUTTON, TAP_PARAM, -1, "TAP"},
    {2, 0, SNAP, KEY_INTERVAL_PARAM, -1, "KEY"},
    {3, 0, SW2, CLOCK_SOURCE_PARAM, -1, "CLK SRC"},
    {4, 0, BUTTON, CLOCK_RESET_PARAM, -1, "RESET"},

    {1.42f, -0.38f, LIGHT, CLOCK_LIGHT, -1, ""},

    {0, 1, SW3, ROUTE_PARAM, -1, "ROUTE"},
    {1, 1, KNOB, CROSSFADE_PARAM, -1, "XFADE"},
    {2, 1, KNOB, CLICK_MIX_PARAM, -1, "CLICK"},
    {3, 1, KNOB, PAN_SPEED_PARAM, -1, "PAN SPD"},
    {4, 1, KNOB, PAN_RANGE_PARAM, -1, "PAN RNG"},

    {0, 3, IN_PORT, IN_L_INPUT, -1, "IN L"},
    {1, 3, IN_PORT, IN_R_INPUT, -1, "IN R"},
    {2, 3, IN_PORT, CV_CROSSFADE_INPUT, -1, "XFADE CV"},
    {3, 3, IN_PORT, CLOCK_INPUT, -1, "CLOCK"},

    {0, 4, OUT_PORT, OUT_L_OUTPUT, -1, "OUT L"},
    {1, 4, OUT_PORT, OUT_R_OUTPUT, -1, "OUT R"},
    {3, 4, OUT_PORT, CLOCK_OUTPUT, -1, "CLK OUT"},
};

} // namespace

struct DoubleDeckWidget : ModuleWidget {
    void placeCells(const Cell* cells, size_t count, float colOrigin, int deck) {
        for (size_t i = 0; i < count; i++) {
            const Cell& c = cells[i];
            const float x = colOrigin + c.col * kColStep;
            const float y = kRow0 + c.row * kRowStep;
            const Vec pos = mm2px(Vec(x, y));
            const int id = c.id + deck;

            switch (c.type) {
                case KNOB:
                    addParam(createParamCentered<RoundSmallBlackKnob>(pos, module, id));
                    break;
                case SNAP:
                    addParam(createParamCentered<RoundSmallBlackKnob>(pos, module, id));
                    break;
                case SW2:
                    addParam(createParamCentered<CKSS>(pos, module, id));
                    break;
                case SW3:
                    addParam(createParamCentered<CKSSThree>(pos, module, id));
                    break;
                case BUTTON:
                    addParam(createParamCentered<VCVButton>(pos, module, id));
                    break;
                case LATCH:
                    addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
                        pos, module, id, c.light + deck));
                    break;
                case LATCH_G:
                    addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
                        pos, module, id, c.light + deck));
                    break;
                case LATCH_R:
                    addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
                        pos, module, id, c.light + deck));
                    break;
                case IN_PORT:
                    addInput(createInputCentered<PJ301MPort>(pos, module, id));
                    break;
                case OUT_PORT:
                    addOutput(createOutputCentered<PJ301MPort>(pos, module, id));
                    break;
                case LIGHT:
                    addChild(createLightCentered<SmallLight<WhiteLight>>(pos, module, id));
                    break;
            }

            if (c.label[0] != '\0') {
                addChild(new PanelLabel(mm2px(Vec(x, y - 6.6f)), c.label));
            }
        }
    }

    DoubleDeckWidget(DoubleDeckModule* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/panels/DoubleDeck.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(
            Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        auto* title = new PanelLabel(mm2px(Vec(142.24f, 8.f)), "DOUBLE DECK");
        title->fontSize = 14.f;
        addChild(title);
        addChild(new PanelLabel(mm2px(Vec(kDeckColA + 6.f, 14.f)), "DECK A"));
        addChild(new PanelLabel(mm2px(Vec(kDeckColB + 6.f, 14.f)), "DECK B"));
        addChild(new PanelLabel(mm2px(Vec(kGlobalCol + 6.f, 14.f)), "GLOBAL"));

        const size_t deckCount = sizeof(kDeckCells) / sizeof(kDeckCells[0]);
        placeCells(kDeckCells, deckCount, kDeckColA, 0);
        placeCells(kDeckCells, deckCount, kDeckColB, 1);
        placeCells(kGlobalCells, sizeof(kGlobalCells) / sizeof(kGlobalCells[0]), kGlobalCol, 0);
    }

    void appendContextMenu(Menu* menu) override {
        auto* m = dynamic_cast<DoubleDeckModule*>(module);
        if (!m) return;

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel(m->deckStatus(0)));
        menu->addChild(createMenuLabel(m->deckStatus(1)));

        // Import: sostituisce la SD card dell'hardware. Nella patch finisce il
        // path, non l'audio, quindi il file deve restare dov'è.
        menu->addChild(new MenuSeparator);
        for (int d = 0; d < 2; d++) {
            const std::string n = d == 0 ? ", deck A" : ", deck B";
            menu->addChild(createMenuItem("Load sample" + n, m->sampleInfo[d],
                                          [=]() { m->loadSampleDialog(d); }));
        }
        for (int d = 0; d < 2; d++) {
            const std::string n = d == 0 ? ", deck A" : ", deck B";
            menu->addChild(createMenuItem("Clear buffer" + n, "", [=]() { m->clearDeck(d); }));
        }

        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Deck A slices in mono", "", &m->sliceMono[0]));
        menu->addChild(createBoolPtrMenuItem("Deck B slices in mono", "", &m->sliceMono[1]));

        // Cambiare lunghezza rialloca il buffer e reinizializza il motore: il
        // contenuto registrato si perde, e sono ~0,8 MB per secondo per deck.
        std::vector<std::string> labels;
        for (float s : kBufferSeconds) labels.push_back(string::f("%.0f s", s));
        menu->addChild(createIndexSubmenuItem(
            "Max loop length", labels,
            [=]() {
                for (size_t i = 0; i < kBufferSeconds.size(); i++) {
                    if (kBufferSeconds[i] == m->bufferSeconds) return i;
                }
                return kBufferSeconds.size(); // nessuna corrispondenza
            },
            [=](size_t i) {
                if (i < kBufferSeconds.size()) m->setBufferSeconds(kBufferSeconds[i]);
            }));
    }
};

Model* modelDoubleDeck = createModel<DoubleDeckModule, DoubleDeckWidget>("DoubleDeck");
