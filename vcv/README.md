# Double Deck

Modulo [VCV Rack](https://vcvrack.com/) — e [MetaRack](https://github.com/lcasarotti/Metarack), il fork accessibile —
ricavato dal firmware dello [Spotykach](https://synthux.academy/store/spotykach) di Synthux Academy:
un looper / sampler / granulatore stereo a due deck.

Il motore DSP è quello originale del firmware, che vive nella radice di questo
repository (`src/core`). Questa directory contiene solo lo strato che lo fa
girare dentro Rack.

## Compilare

Serve il sorgente di VCV Rack (o l'SDK) e, su Windows, la cross-compilazione da
WSL con MinGW.

```sh
git submodule update --init lib/DaisySP     # dalla radice del repo
cd vcv
make RACK_DIR=/percorso/di/Rack
make RACK_DIR=/percorso/di/Rack install
```

## Collaudo

`test/` contiene un harness che esercita il motore **senza Rack**: registrazione
e riproduzione, cambio di velocità durante il play, e isolamento fra due
istanze. Molto più rapido di aprire Rack, e con i sanitizer attivi intercetta
problemi di memoria che a orecchio non si notano.

```sh
make -f test/Makefile run      # veloce
make -f test/Makefile asan     # AddressSanitizer + UndefinedBehaviorSanitizer
```

## Come è fatto

Il motore gira **sempre a 48 kHz in blocchi da 96 campioni**, come
sull'hardware: le costanti DSP del firmware (finestre dei grani, slope,
detector, conversione BPM↔lunghezza del loop) sono tarate su quel rate, e
tenerlo fisso significa non doverne toccare nessuna. L'adattamento al sample
rate di Rack usa due `SampleRateConverter`, bypassati quando Rack è già a
48 kHz.

`src/vcv/` è lo strato di adattamento, deliberatamente sottile:

| File | Ruolo |
|---|---|
| `daisy.h`, `daisy_seed.h` | Shim degli unici due header libDaisy che `src/core` include. `StopwatchTimer` e `System::GetNow` poggiano sul contatore di frame del motore di Rack, quindi restano deterministici anche in rendering offline. |
| `buffer_pool.h` | Rimpiazzo su heap di `SDRAMBuffer`, **per istanza**: sull'hardware i buffer erano `static`, qui due moduli nella stessa patch devono restare separati. |
| `portability.h` | Iniettato con `-include`. `src/core` conta su include transitivi che `arm-none-eabi` fornisce e MinGW no. |

Le modifiche al firmware sono ridotte al minimo e quasi tutte sotto `#ifdef VCV`,
così restano mergeabili con l'upstream di Synthux Academy.

## Licenze

Il plugin è **GPL-3.0-or-later**, obbligatorio per il linking con VCV Rack
(vedi `LICENSE-GPLv3.txt`).

Il firmware Spotykach nella radice del repository resta **MIT**
(© Synthux Academy — vedi `LICENSE` e `CREDITS.md` nella radice), come pure
libDaisy e DaisySP di Electrosmith. MIT è compatibile con la GPLv3, quindi il
binario risultante è distribuito sotto GPL-3.0-or-later.
