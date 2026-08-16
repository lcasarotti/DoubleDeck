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
e riproduzione, cambio di velocità durante il play, isolamento fra due istanze,
e riproduzione di un buffer riempito dall'esterno (la parte di motore che tocca
l'import). Molto più rapido di aprire Rack, e con i sanitizer attivi intercetta
problemi di memoria che a orecchio non si notano.

Attenzione a una trappola: diversi membri del `Core` e del `Generator` non hanno
un valore di default: nel modulo li scrive il primo giro di parametri, nel test
tocca farlo a mano. Il caso peggiore è `_mix_mod`, che senza `mix_mod_in()`
manda l'uscita a NaN — e un NaN non si vede come distorsione, si vede come
silenzio.

```sh
make -f test/Makefile run      # veloce
make -f test/Makefile asan     # AddressSanitizer + UndefinedBehaviorSanitizer
make -f test/Makefile clock    # sincronizzazione al clock esterno
```

`clock` è l'unico dei tre che passa o fallisce da solo: alimenta il motore con
un clock esterno a BPM noto e verifica che i quarti battano quel tempo, per ogni
PPQN dichiarabile. Vedi `IN PPQN` più sotto.

## La superficie di controllo

Sull'hardware ventidue potenziometri servono una settantina di funzioni tramite
cinque layer (BASE, ALT, TAP-HOLD, FLUX, GRIT). Qui i layer non ci sono: ogni
funzione ha il suo parametro, 79 in tutto, dichiarati in
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

**Le etichette sono interfaccia.** Nome, unità, `description` e
`getDisplayValueString` di ogni parametro sono ciò che la vista PARAM pronuncia,
e sono l'unica interfaccia per chi non vede il pannello. Dove una percentuale
non direbbe niente c'è una quantità dedicata: l'intonazione in rapporto e
semitoni, il key interval in quarti e battute, `SIZE` e `POS` in secondi quando
c'è un loop registrato, la velocità del modulatore in hertz o in suddivisioni
del tempo a seconda dello switch di sync, il crossfade come bilanciamento fra i
due deck, la lunghezza del loop in quarti e battute, il Flux in secondi e dB. I
parametri il cui nome non basta portano una `description` di una o due righe.

**Il clock in entrata va dichiarato.** `IN PPQN` dice quanti impulsi per quarto
porta `CLK IN`. Non è un dettaglio: il `Driver` ne ricava il tempo, quindi con
il valore sbagliato il modulo gira a un multiplo esatto del tempo del mittente
(a 4 con un clock da un impulso per quarto suona quattro volte lento). Il
default è **1**, la convenzione più diffusa fra i moduli di clock di Rack;
l'hardware usa 4 sul suo jack, e il MIDI 24. I valori offerti sono i divisori di
48 (`kPPQNIntern`), perché `SynClock` fa `48 / ppqn` in aritmetica intera.
`CLK OUT` resta invece fisso a **24 PPQN**, come sull'hardware: non segue
`IN PPQN`. La prova sta in
[`test/clock_sync.cpp`](test/clock_sync.cpp) (`make -f test/Makefile clock`).

Rispetto all'hardware ci sono tre differenze deliberate: entrambi i deck hanno
tutte e quattro le forme d'onda dell'LFO (sull'hardware erano spartite fra i
due), la direzione di riproduzione è uno switch invece del secondo pad Play
(conservando la semantica «invertire mentre suona non ferma»), e il tempo
ricavato dalla lunghezza del loop si applica con un pulsante `FIT` invece che
muovendo il knob.

Fuori dalla superficie, nel menu contestuale: stato testuale dei due deck,
import di un campione e svuotamento del buffer per deck, slice in mono per
deck, e lunghezza massima del loop (10 / 20 / 42 s), che rialloca il pool e
reinizializza il motore.

## Import di campioni

Prende il posto della SD card dell'hardware. `Load sample, deck A/B` legge un
WAV con [dr_wav](https://github.com/mackron/dr_libs), lo converte **una volta
sola** a 48 kHz stereo (mono raddoppiato, multicanale ridotto ai primi due) e lo
copia in `Buffer::raw()` chiudendo con `set_rec_size()`: è la stessa via che
`src/memory/storage.cpp` usa per caricare un tape. I cue point del chunk `cue `
finiscono nel generatore, quindi un file già affettato arriva affettato — e con
dei cue point la `SIZE` smette di essere elevata al quadrato, per questo dopo
l'import il parametro viene rispinto.

Due conseguenze pratiche:

- **Nella patch va il path, non l'audio**: 42 s stereo sono 16 MB per deck. Se
  il file si sposta, al caricamento della patch resta scritto nel menu che non
  si è aperto, e il path si conserva.
- Un file più lungo del buffer viene **troncato** alla lunghezza scelta nel menu
  (il menu lo dice: `truncated`).

Il lavoro sta sul thread UI. Lettura e conversione — le parti lente — stanno
fuori da `engineMutex`; sotto il lock resta la sola copia, e il thread audio,
che il lock non lo aspetta mai, tace per quei pochi millisecondi.

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

Il pannello è **64 HP**: due riquadri identici per i deck e uno per i globali.
L'SVG contiene solo la grafica, perché nanosvg non rende gli elementi `<text>`
— titolo, nomi delle sezioni ed etichette dei controlli li disegna `PanelLabel`.
Il layout è una tabella di celle su una griglia di 20 × 12,5 mm, applicata due
volte per i due deck: ogni riga è una sezione (TRANSPORT, PLAYHEAD, SHAPE, MOD,
GRIT, FLUX, SEQ, CV) e corrisponde a una fascia colorata del pannello, con il
nome scritto in verticale nel corridoio a sinistra. Le coordinate stanno in due
posti — l'SVG e la griglia di `DoubleDeck.cpp` — e vanno cambiate insieme.

Le modifiche al firmware sono ridotte al minimo e quasi tutte sotto `#ifdef VCV`,
così restano mergeabili con l'upstream di Synthux Academy.

## Licenze

Il plugin è **GPL-3.0-or-later**, obbligatorio per il linking con VCV Rack
(vedi `LICENSE-GPLv3.txt`).

Il firmware Spotykach nella radice del repository resta **MIT**
(© Synthux Academy — vedi `LICENSE` e `CREDITS.md` nella radice), come pure
libDaisy e DaisySP di Electrosmith. `src/dep/dr_wav.h` è di David Reid, public
domain (o MIT-0, a scelta). Tutte licenze compatibili con la GPLv3, quindi il
binario risultante è distribuito sotto GPL-3.0-or-later.
