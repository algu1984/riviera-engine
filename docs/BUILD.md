# Building, testing and integrating Riviera Engine

Run `make deps` once to initialize the pinned source submodules in this repository
and its sibling core checkout. `make deps-check` verifies their recorded revisions
and local patches without network access. Ordinary builds never fetch dependencies:
`make build` builds `build/libriviera-engine.a` and separately builds the core archive
in the sibling repository. The native Jolt archive is built into its cache directory by `make test`; build the
WebAssembly archive explicitly when an application needs it.
Set `CORE_ROOT`, `JOLT_ROOT`, `CGLTF_ROOT`, or `STB_ROOT` to use alternate
checkouts. `make test` runs engine and core native checks, including the generic
physics service smoke test. `make wasm-test` runs core's pthread-enabled portability
tests under Node. The browser renderer uses Emscripten's Dawn WebGPU port and is
integrated by a parent application. `make webgpu-renderer-demo` builds an isolated
procedural workload page that uses the real engine renderer; serve it with
`python3 examples/webgpu-renderer-demo/serve.py`. See [performance demo notes](docs/PERFORMANCE.md). To build Jolt independently, run
`make build/jolt-native/libJolt.a` or `make build/jolt-wasm/libJolt.a`.
The [live WebGPU demo](https://algu1984.github.io/riviera-engine/) and
[Core benchmark results](https://algu1984.github.io/riviera-engine/benchmarks.html)
are published through the manually dispatched `Build and publish WebGPU demo`
Pages workflow; pushes and pull requests do not trigger it.

## Parent build integration

Set `ENGINE_ROOT` and include `$(ENGINE_ROOT)/engine.mk`. The file includes the
sibling `core.mk` by default and exports absolute `ENGINE_SOURCES`, `ENGINE_HEADERS`,
`ENGINE_INCLUDE_FLAGS`, `CORE_SOURCES`, and `CORE_HEADERS`. Compile both source lists
once into an application binary, or link the separately built core and engine
archives with Jolt where needed. It also exports `ENGINE_JOLT_HEADERS` and
`ENGINE_WEB_AUDIO_WORKLET` for the generic PCM output processor. Jolt initialization
is an explicit host-startup call; each physics world is single-owner.

See [design](docs/ENGINE_DESIGN.md), [current status](docs/ENGINE_STATUS.md), and
[dependency pins and patches](dependencies/README.md). Third-party licenses and
notices are retained under `third_party/`.
