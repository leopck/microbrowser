CXX ?= c++
PYTHON ?= python3
GATEWAY_VENV ?= .tools/gateway-venv
GATEWAY_PYTHON ?= $(GATEWAY_VENV)/bin/python


SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)
CURL_CFLAGS := $(shell curl-config --cflags)
CURL_LIBS := $(shell curl-config --libs)

CPPFLAGS += -Iinclude -I. $(SDL_CFLAGS) $(CURL_CFLAGS)
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

.PHONY: all dashboard run test firmware qemu qemu-gpio nrf-gpio nrf-gpio-sim nrf-app nrf-app-sim check-gpio check-conformance check-state gateway web-test clean

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
build/gpio_conformance.mbp: examples/gpio_conformance.mb $(COMPILER_SOURCES)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mbc build $< -o $@


build/runtime_tests: $(CORE_OBJECTS) tests/test_runtime.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@


build/device_builtins_tests: $(CORE_OBJECTS) tests/test_device_builtins.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@
build/peripheral_tests: $(CORE_OBJECTS) tests/test_peripherals.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

build/portable_gpio_tests: $(CORE_OBJECTS) tests/test_portable_gpio.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

check-gpio: build/gpio_conformance.mbp build/portable_gpio_tests
	./build/portable_gpio_tests build/gpio_conformance.mbp


build/peripheral_conformance_tests: $(CORE_OBJECTS) tests/test_peripheral_conformance.cpp tests/peripheral_conformance_fixture.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_OBJECTS) tests/test_peripheral_conformance.cpp -o $@

check-conformance: build/peripheral_conformance_tests
	./build/peripheral_conformance_tests
build/native_persistence_tests: $(CORE_OBJECTS) build/obj/platform/desktop.o tests/test_native_persistence.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDLIBS) -o $@

check-state: build/native_persistence_tests
	./build/native_persistence_tests

test: all dashboard build/ui_test.mbp build/counter.mbp build/runtime_tests build/device_builtins_tests build/peripheral_tests check-gpio check-conformance check-state
	./build/runtime_tests build/dashboard.mbp build/ui_test.mbp
	./build/device_builtins_tests
	./build/peripheral_tests
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py'
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

nrf-gpio:
	firmware/nrf5340_gpio/build.sh

nrf-gpio-sim:
	firmware/nrf5340_gpio/bsim.sh

nrf-app-sim: build/gpio_conformance.mbp
	firmware/nrf5340_app/bsim.sh

nrf-app: build/gpio_conformance.mbp
	firmware/nrf5340_app/build.sh nrf7002dk/nrf5340/cpuapp

qemu: dashboard
	firmware/run.sh --smoke

qemu-gpio:
	firmware/run.sh --smoke --gpio-conformance

$(GATEWAY_VENV)/.installed: gateway/requirements-interactive.txt $(GATEWAY_PYTHON)
	$(GATEWAY_PYTHON) -m pip install --disable-pip-version-check -r $<
	touch $@

$(GATEWAY_PYTHON):
	$(PYTHON) -m venv $(GATEWAY_VENV)

gateway: $(GATEWAY_VENV)/.installed
	$(GATEWAY_PYTHON) -m gateway.server --host 127.0.0.1 --port 8765

web-test: test $(GATEWAY_VENV)/.installed
	$(GATEWAY_PYTHON) -m unittest discover -s gateway/tests -p 'test_*.py' -v

clean:
	rm -rf build

-include $(DEPENDENCIES)
