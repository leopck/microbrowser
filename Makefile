CXX ?= c++
PYTHON ?= python3

SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)
CURL_CFLAGS := $(shell curl-config --cflags)
CURL_LIBS := $(shell curl-config --libs)

CPPFLAGS += -Iinclude $(SDL_CFLAGS) $(CURL_CFLAGS)
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -MD -MP
LDLIBS += $(SDL_LIBS) $(CURL_LIBS)

CORE_SOURCES := $(wildcard core/*.cpp)
DESKTOP_SOURCES := platform/desktop.cpp platform/event_replay.cpp platform/main.cpp
SOURCES := $(CORE_SOURCES) $(DESKTOP_SOURCES)
OBJECTS := $(patsubst %.cpp,build/obj/%.o,$(SOURCES))
DEPENDENCIES := $(OBJECTS:.o=.d)
DASHBOARD_ASSETS := $(wildcard examples/assets/*)
COMPILER_SOURCES := $(wildcard tools/mbc/*.py)
CORE_OBJECTS := $(patsubst %.cpp,build/obj/%.o,$(CORE_SOURCES))

.PHONY: all dashboard run test firmware qemu clean

all: build/microbrowser

build/microbrowser: $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

dashboard: build/dashboard.mbp

run: all dashboard
	./build/microbrowser --package build/dashboard.mbp --scale 2

build/dashboard.mbp: examples/dashboard.mb $(DASHBOARD_ASSETS) $(COMPILER_SOURCES)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mbc build $< -o $@

build/ui_test.mbp: tests/ui_test.mb $(COMPILER_SOURCES)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mbc build $< -o $@

build/counter.mbp: examples/counter.mb $(COMPILER_SOURCES)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mbc build $< -o $@

build/runtime_tests: $(CORE_OBJECTS) tests/test_runtime.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: all dashboard build/ui_test.mbp build/counter.mbp build/runtime_tests
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py'
	./build/runtime_tests build/dashboard.mbp build/ui_test.mbp
	rm -f build/counter-test-state
	MB_STATE_FILE=build/counter-test-state ./build/microbrowser --package build/counter.mbp --headless --size 480x320 --frames 100 --events tests/counter.events --screenshot build/counter.ppm > build/counter-replay.log 2>&1
	cat build/counter-replay.log
	grep 'Counter value: 1' build/counter-replay.log
	! grep -E 'script error|SELFTEST FAIL' build/counter-replay.log
	MB_STATE_FILE=build/dashboard-test-state ./build/microbrowser --package build/dashboard.mbp --headless --frames 180 --events tests/dashboard.events --screenshot build/dashboard.ppm > build/dashboard-replay.log 2>&1
	cat build/dashboard-replay.log
	grep 'dashboard local selftest passed' build/dashboard-replay.log
	! grep -E 'script error|SELFTEST FAIL' build/dashboard-replay.log

firmware: dashboard
	firmware/run.sh --build-only

qemu: dashboard
	firmware/run.sh --build-only
	$(PYTHON) tools/qemu_smoke.py --headless

clean:
	rm -rf build

-include $(DEPENDENCIES)
