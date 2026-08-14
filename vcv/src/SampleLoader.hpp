// Import di campioni: quello che sull'hardware fa la SD card.
//
// Il motore gira fisso a 48 kHz stereo, quindi la conversione avviene **una
// volta sola** qui, al caricamento, e non nel percorso audio. Il risultato è
// già nella forma che `Buffer::raw()` si aspetta: si copia e basta.
#pragma once

#include <rack.hpp>

#include <string>
#include <vector>

#include "core/buffer.h"

namespace dd
{

using namespace rack;

/// Un WAV letto, convertito e pronto da copiare nel buffer di un deck.
struct SampleData {
    /// Audio a 48 kHz stereo, mai più lungo del `maxFrames` richiesto.
    std::vector<spotykach::Buffer::Frame> frames;
    /// Cue point in frame, ordinati e già riscalati a 48 kHz. Al massimo
    /// `kMaxSlicePointCount` (32), quanti ne tiene il `Generator`.
    std::vector<size_t> cuePoints;
    /// Vero se il file era più lungo del buffer del deck.
    bool truncated = false;
    int sourceRate = 0;
    int sourceChannels = 0;
};

/// Legge `path`, lo converte a 48 kHz stereo e lo tronca a `maxFrames`.
/// Ritorna false con un messaggio in `error` se il file non è leggibile.
///
/// Da chiamare **fuori** dal thread audio e senza tenere il lock del motore:
/// leggere e ricampionare un file lungo costa decine di millisecondi.
bool loadSampleFile(const std::string& path, size_t maxFrames, SampleData& out, std::string& error);

} // namespace dd
