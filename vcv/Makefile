RACK_DIR ?= ../../..

FIRMWARE_DIR := ..
DAISYSP_DIR  := ../lib/DaisySP

# Sorgenti del plugin: stanno dentro questa directory, quindi seguono la
# mappatura standard di plugin.mk. dr_wav è C, e resta in un'unità di
# traduzione tutta sua: sono 9000 righe che non hanno motivo di ricompilarsi
# insieme al resto.
SOURCES += $(wildcard src/*.cpp)
SOURCES += $(wildcard src/*.c)

# Sorgenti fuori dalla directory del plugin (il firmware nella radice del repo
# e DaisySP). plugin.mk fa OBJECTS := $(patsubst %,build/%.o,$(SOURCES)), quindi
# "../src/core/x.cpp" diventerebbe "build/../src/core/x.cpp.o": fuori da build/,
# dentro l'albero dei sorgenti, e non ripulito da `make clean`. Li compiliamo
# con una regola nostra che li raccoglie sotto build/ext/.
EXT_SOURCES := $(wildcard $(FIRMWARE_DIR)/src/core/*.cpp)

# cpattern non è referenziato da nessuna parte nel firmware e non include
# <cstdint> (sull'ARM lo otteneva per via transitiva).
EXT_SOURCES := $(filter-out $(FIRMWARE_DIR)/src/core/cpattern.cpp,$(EXT_SOURCES))

# pitchshifter.h e fir.h di DaisySP richiedono CMSIS-DSP, ma sono guardati da
# USE_ARM_DSP che non definiamo, e non hanno sorgenti .cpp.
EXT_SOURCES += $(wildcard $(DAISYSP_DIR)/Source/*/*.cpp)

EXT_OBJECTS := $(patsubst ../%.cpp,build/ext/%.cpp.o,$(EXT_SOURCES))
OBJECTS += $(EXT_OBJECTS)

FLAGS += -DVCV
FLAGS += -Isrc/vcv
FLAGS += -I$(FIRMWARE_DIR)
FLAGS += -I$(FIRMWARE_DIR)/src
FLAGS += -I$(DAISYSP_DIR)/Source
# DaisySP include i propri header per nome semplice ("dsp.h", "delayline.h"):
# servono tutte le sue sottodirectory.
FLAGS += $(addprefix -I,$(wildcard $(DAISYSP_DIR)/Source/*/))

DISTRIBUTABLES += $(wildcard LICENSE*) res

include $(RACK_DIR)/plugin.mk

CXXFLAGS += -std=c++17
# Il firmware si affida a include transitivi che arm-none-eabi fornisce e
# MinGW no. Iniettarli qui evita di modificare ~30 file di src/core. Sta in
# CXXFLAGS e non in FLAGS perché portability.h è C++: dr_wav.c è C.
CXXFLAGS += -include src/vcv/portability.h

build/ext/%.cpp.o: ../%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(EXT_OBJECTS:.o=.d)
