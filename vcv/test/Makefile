# Harness di collaudo del motore, indipendente da Rack e dalla toolchain MinGW.
# Si compila nativo (Linux/WSL) perché serve solo a esercitare il DSP.
#
#   make -f test/Makefile run
#   make -f test/Makefile run ARGS=--no-callbacks

FIRMWARE_DIR := ..
DAISYSP_DIR  := ../lib/DaisySP

SOURCES := test/engine_smoke.cpp
SOURCES += $(wildcard $(FIRMWARE_DIR)/src/core/*.cpp)
SOURCES := $(filter-out $(FIRMWARE_DIR)/src/core/cpattern.cpp,$(SOURCES))
SOURCES += $(wildcard $(DAISYSP_DIR)/Source/*/*.cpp)

FLAGS := -DVCV -std=c++17 -O2 -g
FLAGS += -include src/vcv/portability.h
FLAGS += -Isrc/vcv -I$(FIRMWARE_DIR) -I$(FIRMWARE_DIR)/src -I$(DAISYSP_DIR)/Source
FLAGS += $(addprefix -I,$(wildcard $(DAISYSP_DIR)/Source/*/))

BIN := build/test/engine_smoke

$(BIN): $(SOURCES)
	@mkdir -p build/test
	$(CXX) $(FLAGS) $(SOURCES) -o $@

ASAN_BIN := build/test/engine_asan

$(ASAN_BIN): $(SOURCES)
	@mkdir -p build/test
	$(CXX) $(FLAGS) -O0 -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) -o $@

.PHONY: run asan clean
run: $(BIN)
	./$(BIN) $(ARGS)

asan: $(ASAN_BIN)
	./$(ASAN_BIN) $(ARGS)

clean:
	rm -rf build/test
