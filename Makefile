# NightDriver-Pi; the default build retains Raspberry Pi HUB75 support.
CXXFLAGS ?= -O2 -g
CXXFLAGS += -Wall -Wextra -Wno-unused-parameter -MMD -MP -std=c++20
HOST_OS := $(shell uname -s)
WITH_HUB75 ?= $(if $(filter Darwin,$(HOST_OS)),0,1)
CPPFLAGS += -DWITH_HUB75=$(WITH_HUB75) -I.
LDLIBS += -pthread -lz
OBJECTS = build/$(WITH_HUB75)/main.o build/$(WITH_HUB75)/outputconfig.o build/$(WITH_HUB75)/colorlight.o
MAC_SOURCES = $(if $(filter Darwin,$(HOST_OS)),macethernet.cpp,)
OBJECTS += $(MAC_SOURCES:%.cpp=build/$(WITH_HUB75)/%.o)
RGB_LIB_DISTRIBUTION = rpi-rgb-led-matrix
RGB_LIBRARY = $(RGB_LIB_DISTRIBUTION)/lib/librgbmatrix.a

ifeq ($(WITH_HUB75),1)
CPPFLAGS += -I$(RGB_LIB_DISTRIBUTION)/include
OUTPUT_LIBRARY = $(RGB_LIBRARY)
LDLIBS += -lrt -lm
endif

all: ndpi

# Relink when switching WITH_HUB75; objects live in separate build directories.
ndpi: $(OBJECTS) $(OUTPUT_LIBRARY) FORCE
	$(CXX) $(LDFLAGS) $(OBJECTS) $(OUTPUT_LIBRARY) -o $@ $(LDLIBS)

$(RGB_LIBRARY): FORCE
	$(MAKE) -C $(RGB_LIB_DISTRIBUTION)/lib

build/$(WITH_HUB75)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

TEST_FLAGS = -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I.
TESTS = build/tests/config_test build/tests/colorlight_test build/tests/input_test build/tests/hub75_test build/tests/probe_test build/tests/ram_config_test build/tests/testpattern_test

$(TESTS) build/tests/selection_test: $(wildcard *.h) third_party/picojson.h

build/tests/config_test: tests/config_test.cpp outputconfig.cpp outputconfig.h third_party/picojson.h
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) outputconfig.cpp $< -o $@

build/tests/colorlight_test: tests/colorlight_test.cpp colorlight.cpp outputconfig.cpp colorlight.h outputconfig.h displayoutput.h $(MAC_SOURCES)
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) outputconfig.cpp colorlight.cpp $(MAC_SOURCES) $< -o $@

build/tests/testpattern_test: tests/testpattern_test.cpp testpattern.h displayoutput.h
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) $< -o $@ -pthread

build/tests/input_test: tests/input_test.cpp socketserver.h matrixdraw.h ledbuffer.h globals.h displayoutput.h
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) $< -o $@ -pthread -lz

build/tests/hub75_test: tests/hub75_test.cpp tests/fake_matrix/led-matrix.h hub75output.h displayoutput.h
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -Itests/fake_matrix $< -o $@

build/tests/probe_test: tools/colorlight_probe_test.cpp tools/colorlight_probe.h
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) $< -o $@

build/tests/ram_config_test: tools/colorlight_ram_config_test.cpp tools/colorlight_ram_config.h
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) $< -o $@ -lz

# A matrix-library double throws if main ever creates an RGBMatrix. This
# exercises real CLI/backend selection even on machines with no GPIO library.
build/tests/selection_test: main.cpp colorlight.cpp outputconfig.cpp tests/fake_matrix/led-matrix.h socketserver.h matrixdraw.h ledbuffer.h hub75output.h $(MAC_SOURCES)
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -DWITH_HUB75=1 -Itests/fake_matrix main.cpp colorlight.cpp outputconfig.cpp $(MAC_SOURCES) -o $@ -pthread -lz

test: $(TESTS) build/tests/selection_test
	@set -e; for test in $(TESTS); do ./$$test; done
	python3 tests/capture_audit_test.py
	python3 tests/selection_test.py build/tests/selection_test
	CXX="$(CXX)" python3 tools/colorlight_ram_config_cli_test.py

# Read-only native macOS receiver discovery and flash-page diagnostics.
probe: build/tools/colorlight-probe

build/tools/colorlight-probe: tools/colorlight_probe.cpp tools/colorlight_probe.h macethernet.cpp macethernet.h colorlight.h displayoutput.h
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) tools/colorlight_probe.cpp macethernet.cpp -o $@ -lpcap

ram-config: build/tools/colorlight-ram-config

build/tools/colorlight-ram-config: tools/colorlight_ram_config.cpp tools/colorlight_ram_config.h colorlight.cpp outputconfig.cpp outputconfig.h $(MAC_SOURCES) macethernet.h colorlight.h displayoutput.h displaytiming.h third_party/picojson.h
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) tools/colorlight_ram_config.cpp colorlight.cpp outputconfig.cpp $(MAC_SOURCES) -o $@ -pthread -lz

clean:
	rm -rf build ndpi main.o main.d

-include $(OBJECTS:.o=.d)
.PHONY: all clean test probe ram-config FORCE
FORCE:
