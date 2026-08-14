#include "SampleLoader.hpp"

#include <algorithm>
#include <cmath>

#include "buffer_pool.h" // kEngineSampleRate
#include "core/config.h" // kMaxSlicePointCount
#include "dep/dr_wav.h"

namespace dd
{

namespace
{

/// Quanti frame leggere per giro. Tenerlo piccolo serve a non allocare mai il
/// file intero: un WAV di un'ora sarebbe più di un gigabyte, e a noi ne servono
/// al massimo 42 secondi.
constexpr int kChunk = 4096;

/// Lettura del `cue ` chunk: dr_wav chiama il campo `sampleByteOffset`, ma è la
/// stessa parola a offset 20 del cue point che il firmware legge come Sample
/// Offset (src/memory/wav.cpp), cioè un indice di frame. Nei file PCM — gli
/// unici che ci interessano — è quello che ci scrivono i writer.
size_t cueFrame(const drwav_cue_point& p, const double ratio)
{
    return static_cast<size_t>(std::llround(p.sampleByteOffset * ratio));
}

} // namespace

bool loadSampleFile(const std::string& path, size_t maxFrames, SampleData& out, std::string& error)
{
    out = SampleData();
    error.clear();

    if (maxFrames == 0) {
        error = "the deck has no buffer";
        return false;
    }

    drwav wav;
#if defined ARCH_WIN
    // Su Windows i path viaggiano in UTF-8 dentro Rack ma l'API di sistema
    // vuole UTF-16: senza conversione i nomi non ASCII non si aprono.
    const bool opened = drwav_init_file_with_metadata_w(
        &wav, string::UTF8toUTF16(path).c_str(), 0, NULL);
#else
    const bool opened = drwav_init_file_with_metadata(&wav, path.c_str(), 0, NULL);
#endif
    if (!opened) {
        error = "not a readable WAV file";
        return false;
    }
    DEFER({ drwav_uninit(&wav); });

    if (wav.channels == 0 || wav.sampleRate == 0 || wav.totalPCMFrameCount == 0) {
        error = "the file contains no audio";
        return false;
    }

    out.sourceRate     = (int)wav.sampleRate;
    out.sourceChannels = (int)wav.channels;

    const double ratio = spotykach::kEngineSampleRate / (double)wav.sampleRate;
    const bool resample = wav.sampleRate != (drwav_uint32)spotykach::kEngineSampleRate;

    // Qualità 10 (la massima): la conversione è una tantum, quindi il tempo
    // speso qui non lo paga il percorso audio.
    dsp::SampleRateConverter<2> src;
    src.setQuality(10);
    src.setRates((int)wav.sampleRate, (int)spotykach::kEngineSampleRate);

    const int outChunk = (int)std::ceil(kChunk * ratio) + 32;
    std::vector<float> raw((size_t)kChunk * wav.channels);
    std::vector<dsp::Frame<2>> in(kChunk);
    std::vector<dsp::Frame<2>> resampled(outChunk);

    const size_t expected = (size_t)std::llround(wav.totalPCMFrameCount * ratio);
    out.frames.reserve(std::min(maxFrames, expected + 1));

    auto append = [&](const dsp::Frame<2>* f, int count) {
        for (int i = 0; i < count && out.frames.size() < maxFrames; i++) {
            out.frames.push_back({f[i].samples[0], f[i].samples[1]});
        }
    };

    while (out.frames.size() < maxFrames) {
        const drwav_uint64 read = drwav_read_pcm_frames_f32(&wav, kChunk, raw.data());
        if (read == 0) break;

        for (int i = 0; i < (int)read; i++) {
            const float* f = &raw[(size_t)i * wav.channels];
            // Mono raddoppiato, multicanale ridotto ai primi due: è la
            // convenzione di qualsiasi lettore, e il motore è stereo.
            in[i].samples[0] = f[0];
            in[i].samples[1] = wav.channels == 1 ? f[0] : f[1];
        }

        if (!resample) {
            append(in.data(), (int)read);
            continue;
        }

        // speex può consumare meno input di quanto gliene diamo se l'uscita si
        // riempie prima: si insiste finché il chunk non è esaurito.
        int consumed = 0;
        while (consumed < (int)read) {
            int inLen  = (int)read - consumed;
            int outLen = outChunk;
            src.process(in.data() + consumed, &inLen, resampled.data(), &outLen);
            if (inLen == 0 && outLen == 0) break; // nessun progresso: meglio uscire
            consumed += inLen;
            append(resampled.data(), outLen);
        }
    }

    if (out.frames.empty()) {
        error = "the file contains no audio";
        return false;
    }

    out.truncated = expected > out.frames.size();

    for (drwav_uint32 i = 0; i < wav.metadataCount; i++) {
        const drwav_metadata& md = wav.pMetadata[i];
        if (md.type != drwav_metadata_type_cue) continue;
        for (drwav_uint32 c = 0; c < md.data.cue.cuePointCount; c++) {
            const size_t frame = cueFrame(md.data.cue.pCuePoints[c], ratio);
            // Fuori dall'audio caricato non significano niente: un file
            // troncato perde i suoi cue point di coda.
            if (frame < out.frames.size()) out.cuePoints.push_back(frame);
        }
    }

    // Il `Generator` legge i cue point per indice e assume che crescano
    // (`_cue_points[start_idx]` … `_cue_points[end_idx]`). Il firmware leggeva
    // solo file scritti da sé, già ordinati; qui il file lo sceglie l'utente,
    // quindi ordiniamo noi.
    std::sort(out.cuePoints.begin(), out.cuePoints.end());
    out.cuePoints.erase(std::unique(out.cuePoints.begin(), out.cuePoints.end()),
                        out.cuePoints.end());
    if (out.cuePoints.size() > spotykach::kMaxSlicePointCount) {
        out.cuePoints.resize(spotykach::kMaxSlicePointCount);
    }

    return true;
}

} // namespace dd
