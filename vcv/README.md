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

## La superficie di controllo

Sull'hardware ventidue potenziometri servono una settantina di funzioni tramite
cinque layer (BASE, ALT, TAP-HOLD, FLUX, GRIT). Qui i layer non ci sono: ogni
funzione ha il suo parametro, 78 in tutto, dichiarati in
[`src/ParamMap.cpp`](src/ParamMap.cpp) e spinti nel motore da
`DoubleDeckModule::updateDeckParams` / `updateGlobalParams`, una volta per
blocco — cioè alla stessa cadenza del main loop del firmware.

Ne derivano tre regole che vale la pena conoscere prima di toccare il codice.

**I parametri sono la verità, non il motore.** Un parametro viene spinto solo
quando cambia (`moved()`); quando cambia il *contesto* che ne determina
l'effetto — il modo del deck, la quantizzazione dell'intonazione, il modo del
Grit — lo si marca da rispingere (`repush()`). Sull'hardware, cambiando il modo
del Grit, erano i knob a saltare al valore del nuovo modo; qui succede il
contrario.

**I latch rispecchiano il motore.** `PLAY`, `REC`, `GRIT ON`, `FLUX ON` e
`SEQ ARM` passano tutti da `syncLatch()`: la variazione è una *richiesta*, e
subito dopo il latch viene riallineato allo stato reale. Serve perché il motore
non obbedisce sempre — `toggle_play` non fa nulla a buffer vuoto, e in Slice
accoda l'avvio al prossimo key tick — e perché in MetaRack lo screen reader
legge la `ParamQuantity`, non il motore. Un avvio in coda conta come già attivo
(il pulsante non scatta indietro sotto le dita) e si distingue dalla luminosità
della spia.

**Le etichette sono interfaccia.** Nome, unità e `getDisplayValueString` di ogni
parametro sono ciò che la vista PARAM pronuncia: l'intonazione è in rapporto e
semitoni, il key interval in quarti e battute, la size in secondi quando c'è un
loop registrato, il Flux in secondi e dB.

Rispetto all'hardware ci sono tre differenze deliberate: entrambi i deck hanno
tutte e quattro le forme d'onda dell'LFO (sull'hardware erano spartite fra i
due), la direzione di riproduzione è uno switch invece del secondo pad Play
(conservando la semantica «invertire mentre suona non ferma»), e il tempo
ricavato dalla lunghezza del loop si applica con un pulsante `FIT` invece che
muovendo il knob.

Fuori dalla superficie, nel menu contestuale: stato testuale dei due deck,
slice in mono per deck, e lunghezza massima del loop (10 / 20 / 42 s), che
rialloca il pool e reinizializza il motore.

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

Il pannello è provvisorio (fase 5): l'SVG contiene solo il fondo, perché nanosvg
non rende gli elementi `<text>` — titolo ed etichette li disegna `PanelLabel`.
Il layout è una tabella di celle su griglia, applicata due volte per i due deck.

Le modifiche al firmware sono ridotte al minimo e quasi tutte sotto `#ifdef VCV`,
così restano mergeabili con l'upstream di Synthux Academy.

## Licenze

Il plugin è **GPL-3.0-or-later**, obbligatorio per il linking con VCV Rack
(vedi `LICENSE-GPLv3.txt`).

Il firmware Spotykach nella radice del repository resta **MIT**
(© Synthux Academy — vedi `LICENSE` e `CREDITS.md` nella radice), come pure
libDaisy e DaisySP di Electrosmith. MIT è compatibile con la GPLv3, quindi il
binario risultante è distribuito sotto GPL-3.0-or-later.
