CXX ?= clang++
AR ?= ar
EMXX ?= em++
# Resolve through the shell so non-executable SDK cmake directories are skipped.
CMAKE ?= $(shell command -v cmake || printf cmake)
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pthread
.DEFAULT_GOAL := all
ENGINE_ROOT := $(abspath .)
CORE_ROOT ?= $(abspath $(ENGINE_ROOT)/../riviera-engine-core)
JOLT_ROOT ?= $(ENGINE_ROOT)/third_party/jolt
CGLTF_ROOT ?= $(ENGINE_ROOT)/third_party/cgltf
STB_ROOT ?= $(ENGINE_ROOT)/third_party/stb
ENGINE_DEPENDENCIES_MANIFEST := $(ENGINE_ROOT)/dependencies/lock.json
ifeq ($(wildcard $(CORE_ROOT)/core.mk),)
  ifeq ($(MAKECMDGOALS),)
    $(error Riviera Engine Core is required for build/test. Set CORE_ROOT to its checkout.)
  endif
  ifneq ($(strip $(filter-out format format-check clean,$(MAKECMDGOALS))),)
    $(error Riviera Engine Core is required for build/test. Set CORE_ROOT to its checkout.)
  endif
  ENGINE_SOURCES :=
  ENGINE_HEADERS :=
  ENGINE_INCLUDE_FLAGS :=
  ENGINE_XENIUM_HEADERS :=
  ENGINE_JOLT_HEADERS :=
  ENGINE_JOLT_FILES :=
  ENGINE_JOLT_CPPFLAGS :=
  ENGINE_CORE_ARCHIVE := $(CORE_ROOT)/build/libriviera-engine-core.a
  CORE_SOURCES :=
  CORE_HEADERS :=
  CORE_XENIUM_HEADERS :=
  CORE_INCLUDE_FLAGS :=
else
  include engine.mk
endif
ENGINE_DEPENDENCIES_TOOL ?= $(CORE_ROOT)/tools/dependencies.py

ENGINE_CPU_SOURCES := $(ENGINE_ROOT)/src/jobs/executor.cpp \
  $(ENGINE_ROOT)/src/graphics/frame_graph/frame_graph.cpp \
  $(ENGINE_ROOT)/src/scene/scene.cpp $(ENGINE_ROOT)/src/ui/canvas.cpp \
  $(ENGINE_ROOT)/src/particles/particle_system.cpp \
  $(ENGINE_ROOT)/src/physics/physics_world.cpp \
  $(ENGINE_ROOT)/src/physics/scheduler_job_system.cpp
ENGINE_OBJECTS := $(patsubst $(ENGINE_ROOT)/src/%.cpp,build/%.o,$(ENGINE_CPU_SOURCES))
TEST_FLAGS := $(CXXFLAGS) $(ENGINE_INCLUDE_FLAGS) -I$(ENGINE_ROOT)/tests
JOLT_FLAGS := -G Ninja -DCMAKE_BUILD_TYPE=Distribution -DTARGET_UNIT_TESTS=OFF \
  -DTARGET_HELLO_WORLD=OFF -DTARGET_SAMPLES=OFF -DTARGET_VIEWER=OFF \
  -DTARGET_PERFORMANCE_TEST=OFF -DENABLE_INSTALL=OFF -DGENERATE_DEBUG_SYMBOLS=OFF \
  -DINTERPROCEDURAL_OPTIMIZATION=OFF -DENABLE_ALL_WARNINGS=OFF -DJPH_USE_DX12=OFF \
  -DJPH_USE_VK=OFF -DJPH_USE_MTL=OFF -DJPH_USE_CPU_COMPUTE=OFF

.PHONY: all build test deps deps-check require-dependencies engine-test scene-test ecs-integration-test ui-test particle-test physics-world-test physics-scheduler-test core-test jolt-wasm-pthread web-worker-probe web-compute-smoke \
  ecs-test runtime-test hierarchy-test jobs-test xenium-portability-test wasm-test ecs-wasm-test \
  clean format format-check demo-workloads-test physics-demo-test demo-test

all: build test

build: require-dependencies build/libriviera-engine.a $(ENGINE_CORE_ARCHIVE)

deps:
	$(MAKE) -C "$(CORE_ROOT)" deps
	python3 "$(ENGINE_DEPENDENCIES_TOOL)" --manifest "$(ENGINE_DEPENDENCIES_MANIFEST)" --repository "$(ENGINE_ROOT)"
deps-check:
	$(MAKE) -C "$(CORE_ROOT)" deps-check
	python3 "$(ENGINE_DEPENDENCIES_TOOL)" --manifest "$(ENGINE_DEPENDENCIES_MANIFEST)" --repository "$(ENGINE_ROOT)" --check
require-dependencies:
	python3 "$(ENGINE_DEPENDENCIES_TOOL)" --manifest "$(ENGINE_DEPENDENCIES_MANIFEST)" --repository "$(ENGINE_ROOT)" --check

build/libriviera-engine.a: $(ENGINE_OBJECTS) Makefile engine.mk $(ENGINE_DEPENDENCIES_MANIFEST) dependencies/patches/stb-truetype.patch | require-dependencies
	mkdir -p build
	rm -f $@
	$(AR) rcs $@ $(ENGINE_OBJECTS)

$(ENGINE_CORE_ARCHIVE): $(CORE_SOURCES) $(CORE_HEADERS) $(CORE_XENIUM_HEADERS)
	$(MAKE) -C "$(CORE_ROOT)" build

build/%.o: $(ENGINE_ROOT)/src/%.cpp $(ENGINE_HEADERS) $(CORE_HEADERS) \
  $(ENGINE_JOLT_HEADERS) $(ENGINE_CGLTF_HEADER) $(ENGINE_STB_HEADERS) \
  $(ENGINE_DEPENDENCIES_MANIFEST) dependencies/patches/stb-truetype.patch Makefile engine.mk
	mkdir -p $(dir $@)
	$(CXX) $(TEST_FLAGS) $(ENGINE_JOLT_CPPFLAGS) -c $< -o $@

build/engine-test: $(ENGINE_ROOT)/src/jobs/executor.cpp \
  $(ENGINE_ROOT)/src/graphics/frame_graph/frame_graph.cpp \
  $(CORE_SOURCES) tests/runtime.cpp $(ENGINE_HEADERS) $(CORE_HEADERS) Makefile engine.mk
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $(filter %.cpp,$^) -o $@
engine-test: build/engine-test
	./build/engine-test

build/scene-test: $(ENGINE_ROOT)/src/scene/scene.cpp $(CORE_SOURCES) tests/scene.cpp \
  $(ENGINE_HEADERS) $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile engine.mk
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $(filter %.cpp,$^) -o $@
scene-test: build/scene-test
	./build/scene-test

build/ecs-integration-test: $(ENGINE_ROOT)/src/jobs/executor.cpp $(CORE_SOURCES) tests/ecs_integration.cpp \
  $(ENGINE_HEADERS) $(CORE_HEADERS) Makefile engine.mk
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $(filter %.cpp,$^) -o $@
ecs-integration-test: build/ecs-integration-test
	./build/ecs-integration-test

build/ui-test: $(ENGINE_ROOT)/src/ui/canvas.cpp tests/ui.cpp $(ENGINE_HEADERS) \
  $(ENGINE_STB_HEADERS) Makefile engine.mk
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $(filter %.cpp,$^) -o $@
ui-test: require-dependencies build/ui-test
	./build/ui-test

build/particle-test: $(ENGINE_ROOT)/src/particles/particle_system.cpp tests/particles.cpp $(ENGINE_HEADERS) $(CORE_HEADERS) Makefile engine.mk
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $(filter %.cpp,$^) -o $@
particle-test: build/particle-test
	./build/particle-test

build/jolt-wasm/libJolt.a: $(ENGINE_JOLT_FILES) $(ENGINE_DEPENDENCIES_MANIFEST) Makefile engine.mk | require-dependencies
	emcmake $(CMAKE) -S $(JOLT_ROOT)/Build -B build/jolt-wasm $(JOLT_FLAGS)
	$(CMAKE) --build build/jolt-wasm --target Jolt -j 6
$(ENGINE_JOLT_WASM_PTHREAD_ARCHIVE): $(ENGINE_JOLT_FILES) $(ENGINE_DEPENDENCIES_MANIFEST) Makefile engine.mk | require-dependencies
	emcmake $(CMAKE) -S $(JOLT_ROOT)/Build -B $(dir $(ENGINE_JOLT_WASM_PTHREAD_ARCHIVE)) $(JOLT_FLAGS) \
	  -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread
	$(CMAKE) --build $(dir $(ENGINE_JOLT_WASM_PTHREAD_ARCHIVE)) --target Jolt -j 6
build/jolt-wasm-pthread/libJolt.a: $(ENGINE_JOLT_WASM_PTHREAD_ARCHIVE)
jolt-wasm-pthread: $(ENGINE_JOLT_WASM_PTHREAD_ARCHIVE)
build/web-worker-probe.html: tests/web_worker_probe.cpp tests/web_worker_probe_shell.html \
	$(ENGINE_ROOT)/src/platform/web/host.cpp $(ENGINE_ROOT)/src/platform/web/gpu_surface.cpp \
	$(ENGINE_HEADERS) engine.mk Makefile
	mkdir -p build
	$(EMXX) -std=c++20 -O1 -Wall -Wextra $(ENGINE_WEB_THREAD_CPPFLAGS) \
	  $(ENGINE_WEB_THREAD_LDFLAGS) -sASSERTIONS=1 $(ENGINE_INCLUDE_FLAGS) \
	  tests/web_worker_probe.cpp $(ENGINE_ROOT)/src/platform/web/host.cpp \
	  $(ENGINE_ROOT)/src/platform/web/gpu_surface.cpp \
	  --shell-file tests/web_worker_probe_shell.html -o $@
web-worker-probe: build/web-worker-probe.html
build/jolt-native/libJolt.a: $(ENGINE_JOLT_FILES) $(ENGINE_DEPENDENCIES_MANIFEST) Makefile engine.mk | require-dependencies
	$(CMAKE) -S $(JOLT_ROOT)/Build -B build/jolt-native $(JOLT_FLAGS)
	$(CMAKE) --build build/jolt-native --target Jolt -j 6

build/physics-world-test: tests/physics_world.cpp $(ENGINE_ROOT)/src/jobs/executor.cpp \
  $(ENGINE_ROOT)/src/physics/physics_world.cpp $(ENGINE_ROOT)/src/physics/scheduler_job_system.cpp \
  $(CORE_SOURCES) $(ENGINE_HEADERS) $(ENGINE_JOLT_HEADERS) Makefile engine.mk build/jolt-native/libJolt.a
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $(filter %.cpp,$^) \
	  -DJPH_OBJECT_STREAM -DJPH_NO_DEBUG build/jolt-native/libJolt.a -o $@
physics-world-test: require-dependencies build/physics-world-test
	./build/physics-world-test

build/physics-scheduler-test: tests/physics_scheduler.cpp $(ENGINE_ROOT)/src/jobs/executor.cpp \
  $(ENGINE_ROOT)/src/physics/physics_world.cpp $(ENGINE_ROOT)/src/physics/scheduler_job_system.cpp \
  $(CORE_SOURCES) $(ENGINE_HEADERS) $(ENGINE_JOLT_HEADERS) Makefile engine.mk build/jolt-native/libJolt.a
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $(filter %.cpp,$^) \
	  -DJPH_OBJECT_STREAM -DJPH_NO_DEBUG build/jolt-native/libJolt.a -o $@
physics-scheduler-test: require-dependencies build/physics-scheduler-test
	./build/physics-scheduler-test

core-test:
	$(MAKE) -C "$(CORE_ROOT)" test
ecs-test:
	$(MAKE) -C "$(CORE_ROOT)" ecs-test
runtime-test:
	$(MAKE) -C "$(CORE_ROOT)" runtime-test
hierarchy-test:
	$(MAKE) -C "$(CORE_ROOT)" hierarchy-test
jobs-test:
	$(MAKE) -C "$(CORE_ROOT)" jobs-test
xenium-portability-test:
	$(MAKE) -C "$(CORE_ROOT)" xenium-portability-test
wasm-test:
	$(MAKE) -C "$(CORE_ROOT)" wasm-test
ecs-wasm-test:
	$(MAKE) -C "$(CORE_ROOT)" wasm-test

test: require-dependencies engine-test scene-test ecs-integration-test ui-test particle-test physics-world-test physics-scheduler-test demo-test core-test


build/demo-workloads-test: src/jobs/executor.cpp $(CORE_SOURCES) \
  examples/webgpu-renderer-demo/ecs_demo.cpp examples/webgpu-renderer-demo/jobs_demo.cpp \
  tests/demo_workloads.cpp $(ENGINE_HEADERS) $(CORE_HEADERS) Makefile engine.mk
	mkdir -p build
	$(CXX) $(TEST_FLAGS) -UNDEBUG -I$(ENGINE_ROOT) $(filter %.cpp,$^) -o $@
demo-workloads-test: build/demo-workloads-test
	./build/demo-workloads-test

build/physics-demo-test: tests/physics_demo.cpp src/jobs/executor.cpp \
  src/physics/physics_world.cpp src/physics/scheduler_job_system.cpp \
  examples/webgpu-renderer-demo/physics_demo.cpp $(CORE_SOURCES) \
  $(ENGINE_HEADERS) $(CORE_HEADERS) $(ENGINE_JOLT_HEADERS) Makefile engine.mk build/jolt-native/libJolt.a
	mkdir -p build
	$(CXX) $(TEST_FLAGS) -UNDEBUG -DJPH_OBJECT_STREAM -DJPH_NO_DEBUG $(filter %.cpp,$^) \
	  build/jolt-native/libJolt.a -o $@
physics-demo-test: require-dependencies build/physics-demo-test
	./build/physics-demo-test
demo-test: require-dependencies demo-workloads-test physics-demo-test

-include tools/format.mk

clean:
	rm -rf build

WEBGPU_DEMO_DIR := $(ENGINE_ROOT)/build/webgpu-renderer-demo
WEBGPU_DEMO_PAGE_SOURCES := examples/webgpu-renderer-demo/index.html \
  examples/webgpu-renderer-demo/benchmarks.html \
  examples/webgpu-renderer-demo/coi-serviceworker.js \
  examples/webgpu-renderer-demo/style.css docs/PERFORMANCE.md LICENSE \
  third_party/cgltf/LICENSE third_party/stb/LICENSE third_party/jolt/LICENSE \
  $(CORE_ROOT)/third_party/xenium/LICENSE \
  $(CORE_ROOT)/benchmarks/results/native-sample.json
.PHONY: webgpu-renderer-demo
WEBGPU_DEMO_CPP_SOURCES := examples/webgpu-renderer-demo/main.cpp \
  examples/webgpu-renderer-demo/ecs_demo.cpp examples/webgpu-renderer-demo/jobs_demo.cpp \
  examples/webgpu-renderer-demo/physics_demo.cpp src/jobs/executor.cpp \
  src/physics/physics_world.cpp src/physics/scheduler_job_system.cpp \
  src/renderer.cpp src/platform/web/host.cpp src/platform/web/gpu_surface.cpp \
  src/graphics/frame_graph/frame_graph.cpp $(CORE_SOURCES)
$(WEBGPU_DEMO_DIR)/index.js: $(WEBGPU_DEMO_CPP_SOURCES) \
  examples/webgpu-renderer-demo/workload.hpp examples/webgpu-renderer-demo/ecs_demo.hpp \
  examples/webgpu-renderer-demo/jobs_demo.hpp examples/webgpu-renderer-demo/physics_demo.hpp \
  $(ENGINE_HEADERS) $(CORE_HEADERS) $(ENGINE_JOLT_HEADERS) $(ENGINE_JOLT_WASM_PTHREAD_ARCHIVE) \
  engine.mk Makefile
	mkdir -p $(WEBGPU_DEMO_DIR)
	$(EMXX) -std=c++20 -O2 -Wall -Wextra $(ENGINE_WEB_THREAD_CPPFLAGS) \
	  $(ENGINE_WEB_THREAD_LDFLAGS) -sASSERTIONS=1 -sINITIAL_MEMORY=67108864 \
	  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=536870912 $(ENGINE_INCLUDE_FLAGS) \
	  $(ENGINE_JOLT_CPPFLAGS) $(WEBGPU_DEMO_CPP_SOURCES) \
	  $(ENGINE_JOLT_WASM_PTHREAD_ARCHIVE) -o $@
$(WEBGPU_DEMO_DIR)/index.html: $(WEBGPU_DEMO_PAGE_SOURCES) $(WEBGPU_DEMO_DIR)/index.js
	cp examples/webgpu-renderer-demo/index.html $@
	cp examples/webgpu-renderer-demo/coi-serviceworker.js $(WEBGPU_DEMO_DIR)/coi-serviceworker.js
	cp examples/webgpu-renderer-demo/style.css $(WEBGPU_DEMO_DIR)/style.css
	cp docs/PERFORMANCE.md $(WEBGPU_DEMO_DIR)/PERFORMANCE.md
	cp examples/webgpu-renderer-demo/benchmarks.html $(WEBGPU_DEMO_DIR)/benchmarks.html
	mkdir -p $(WEBGPU_DEMO_DIR)/benchmarks
	cp $(CORE_ROOT)/benchmarks/results/native-sample.json $(WEBGPU_DEMO_DIR)/benchmarks/native-sample.json
	cp LICENSE $(WEBGPU_DEMO_DIR)/LICENSE
	mkdir -p $(WEBGPU_DEMO_DIR)/licenses
	cp third_party/cgltf/LICENSE $(WEBGPU_DEMO_DIR)/licenses/cgltf-LICENSE
	cp third_party/stb/LICENSE $(WEBGPU_DEMO_DIR)/licenses/stb-LICENSE
	cp third_party/jolt/LICENSE $(WEBGPU_DEMO_DIR)/licenses/jolt-LICENSE
	cp $(CORE_ROOT)/third_party/xenium/LICENSE $(WEBGPU_DEMO_DIR)/licenses/xenium-LICENSE
webgpu-renderer-demo: $(WEBGPU_DEMO_DIR)/index.html $(WEBGPU_DEMO_DIR)/index.js
	test -s $(WEBGPU_DEMO_DIR)/index.wasm

WEBGPU_COMPUTE_SMOKE_DIR := $(ENGINE_ROOT)/build/webgpu-compute-smoke
WEBGPU_COMPUTE_SMOKE_SOURCES := examples/web-compute-smoke.cpp src/compute_context.cpp
$(WEBGPU_COMPUTE_SMOKE_DIR)/index.html: $(WEBGPU_COMPUTE_SMOKE_SOURCES) $(ENGINE_HEADERS) engine.mk Makefile
	mkdir -p $(WEBGPU_COMPUTE_SMOKE_DIR)
	$(EMXX) -std=c++20 -O2 -Wall -Wextra $(ENGINE_WEB_COMPUTE_COMPILE_FLAGS) \
	  $(ENGINE_WEB_COMPUTE_LINK_FLAGS) $(ENGINE_INCLUDE_FLAGS) \
	  $(WEBGPU_COMPUTE_SMOKE_SOURCES) -o $@
web-compute-smoke: $(WEBGPU_COMPUTE_SMOKE_DIR)/index.html
	test -s $(WEBGPU_COMPUTE_SMOKE_DIR)/index.wasm
