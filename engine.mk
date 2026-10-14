# Riviera Engine integration for parent builds. Set ENGINE_ROOT before including this file.
ENGINE_ROOT ?= $(dir $(lastword $(MAKEFILE_LIST)))
ENGINE_ROOT := $(abspath $(ENGINE_ROOT))
CORE_ROOT ?= $(abspath $(ENGINE_ROOT)/../riviera-engine-core)
JOLT_ROOT ?= $(ENGINE_ROOT)/third_party/jolt
CGLTF_ROOT ?= $(ENGINE_ROOT)/third_party/cgltf
STB_ROOT ?= $(ENGINE_ROOT)/third_party/stb
ENGINE_DEPENDENCIES_MANIFEST := $(ENGINE_ROOT)/dependencies/lock.json
ENGINE_DEPENDENCIES_TOOL := $(CORE_ROOT)/tools/dependencies.py
include $(CORE_ROOT)/core.mk
ENGINE_SOURCES := $(shell find $(ENGINE_ROOT)/src -name '*.cpp' -type f | sort)
ENGINE_HEADERS := $(shell find $(ENGINE_ROOT)/include -type f \( -name '*.hpp' -o -name '*.h' \) | sort)
ENGINE_INCLUDE_FLAGS := -I$(ENGINE_ROOT)/include -I$(JOLT_ROOT) -I$(CGLTF_ROOT) -I$(STB_ROOT) $(CORE_INCLUDE_FLAGS)
ENGINE_XENIUM_HEADERS := $(shell find $(CORE_ROOT)/third_party/xenium -type f \( -name '*.hpp' -o -name '*.h' \) | sort)
ENGINE_JOLT_HEADERS := $(shell find $(JOLT_ROOT)/Jolt -type f -name '*.h' 2>/dev/null | sort)
ENGINE_JOLT_FILES := $(shell find $(JOLT_ROOT) -type f 2>/dev/null | sort)
ENGINE_CGLTF_HEADER := $(CGLTF_ROOT)/cgltf.h
ENGINE_STB_HEADERS := $(STB_ROOT)/stb_image.h $(STB_ROOT)/stb_truetype.h
ENGINE_JOLT_CPPFLAGS := -DJPH_OBJECT_STREAM -DNDEBUG
ENGINE_CORE_ARCHIVE := $(CORE_ROOT)/build/libriviera-engine-core.a
ENGINE_WEB_AUDIO_WORKLET := $(ENGINE_ROOT)/web/engine-worklet.js
# Use the same pthread setting when compiling the application and every
# WebAssembly static library that contributes to its shared-memory module.
ENGINE_WEB_THREAD_CPPFLAGS := -pthread
ENGINE_WEB_THREAD_LDFLAGS := -pthread -sOFFSCREENCANVAS_SUPPORT=1 \
  -sPTHREAD_POOL_SIZE='Math.max(2, (typeof navigator !== "undefined" && Number.isFinite(navigator.hardwareConcurrency) && navigator.hardwareConcurrency > 0) ? Math.floor(navigator.hardwareConcurrency) : 8)' \
  --use-port=emdawnwebgpu
ENGINE_WEB_COMPILE_FLAGS := $(ENGINE_WEB_THREAD_CPPFLAGS)
ENGINE_WEB_LINK_FLAGS := $(ENGINE_WEB_THREAD_LDFLAGS)
# Headless WebGPU compute clients do not require pthreads or OffscreenCanvas.
ENGINE_WEB_COMPUTE_COMPILE_FLAGS := --use-port=emdawnwebgpu
ENGINE_WEB_COMPUTE_LINK_FLAGS := --use-port=emdawnwebgpu
ENGINE_JOLT_WASM_PTHREAD_ARCHIVE := $(ENGINE_ROOT)/build/jolt-wasm-pthread/libJolt.a
