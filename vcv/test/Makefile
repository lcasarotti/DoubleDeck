# Harness di collaudo del motore, indipendente da Rack e dalla toolchain MinGW.
# Si compila nativo (Linux/WSL) perché serve solo a esercitare il DSP.
#
#   make -f test/Makefile run
#   make -f test/Makefile run ARGS=--no-callbacks
#   make -f test/Makefile clock

FIRMWARE_DIR := ..
DAISYSP_DIR  := ../lib/DaisySP

ENGINE_SOURCES := $(wildcard $(FIRMWARE_DIR)/src/core/*.cpp)
ENGINE_SOURCES := $(filter-out $(FIRMWARE_DIR)/src/core/cpattern.cpp,$(ENGINE_SOURCES))
ENGINE_SOURCES += $(wildcard $(DAISYSP_DIR)/Source/*/*.cpp)

SOURCES := test/engine_smoke.cpp $(ENGINE_SOURCES)

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

CLOCK_BIN := build/test/clock_sync

$(CLOCK_BIN): test/clock_sync.cpp $(ENGINE_SOURCES)
	@mkdir -p build/test
	$(CXX) $(FLAGS) test/clock_sync.cpp $(ENGINE_SOURCES) -o $@

.PHONY: run asan clock clean
run: $(BIN)
	./$(BIN) $(ARGS)

asan: $(ASAN_BIN)
	./$(ASAN_BIN) $(ARGS)

clock: $(CLOCK_BIN)
	./$(CLOCK_BIN) $(ARGS)

clean:
	rm -rf build/test
