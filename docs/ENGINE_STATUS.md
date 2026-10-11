# Engine status

Riviera Engine is the application-facing C++20 runtime around the portable
[riviera-engine-core library](../../riviera-engine-core/README.md). Both repositories
use the `engine/...` include path and `engine` namespace, with one owner for each
public header path. The engine imports core source/header lists through `core.mk`.
Core behavior and guarantees are described in the
[core design](../../riviera-engine-core/docs/CORE_DESIGN.md) and
[core boundary](../../riviera-engine-core/docs/CORE_BOUNDARY.md).

This repository currently provides:

- A three-lane browser host: the main thread owns DOM callbacks, viewport reads,
  and privileged services; a render pthread owns WebGPU and an OffscreenCanvas;
  a separate simulation pthread owns its timer-driven pump. Input uses a bounded
  render queue with FocusLost overflow recovery and an optional immediate
  main-thread observer for simulation-owned physical input. Viewport state is
  atomically published to workers.
- A WebGPU renderer with a forward scene path, shadows, sky, MSAA resolve and UI
  passes, plus direct glTF geometry/material and PNG/JPEG texture import. Public
  renderer material and mesh interfaces still expose raw WebGPU handles.
- A frame graph that validates pass dependencies, resource versions and attachment
  rules; prunes unneeded passes; and assigns compatible nonoverlapping transient
  texture lifetimes to logical slots. WebGPU provides implicit transitions; the
  engine does not implement explicit Vulkan-style barriers or native memory aliasing.
- An executor that delegates task scheduling to core and supplies optional native
  host workers. WebAssembly workers require a pthread-enabled build; the no-pthread
  build stays serial. The executor exposes its owned core job scheduler so
  `SystemScheduler` can submit to the same worker pool without creating another
  one. The current race application keeps its scene preparation in the existing
  task graph and uses a serial executor with Jolt's single-threaded job system.
- A graphics-facing `Scene` that stores its rendering components in one core ECS
  world and extracts caller-owned draw records. Core owns dense ECS storage, signature-based system scheduling, structural
  command application, and generic transform hierarchy algorithms. Scene methods
  remain synchronous consumers; this does not imply the race game has migrated
  every subsystem to the ECS system scheduler.
- A generic Jolt `PhysicsWorld` that owns `PhysicsSystem`, a temporary allocator, and
  a single-threaded Jolt job system. Callers provide layer filters and own bodies,
  shapes, constraints, and game-specific simulation policy. Each world is single-owner.
- Reusable C++ canvas/layout UI and a browser AudioWorklet for generic planar stereo
  PCM transport. UI policy and audio synthesis/mixing remain application-owned.

Jolt initialization is an explicit host-startup operation and must precede world and
worker creation. The physics adapter exposes Jolt types directly and is not a
runtime-switchable backend abstraction. The engine does not define game vehicle
controllers, tire models, collision-layer meaning, or fixed-step policy. See
[engine design](ENGINE_DESIGN.md) for the ownership rationale and current limits.

## Build and validation

`make deps-check` verifies local submodule pins and patches without fetching. Normal
builds also validate these local checkouts. `make test` runs the engine native tests,
the generic physics smoke test, and the sibling core native suite. The engine tests
cover executor integration, frame-graph validation, scene extraction, UI behavior,
and a Jolt world step. `make wasm-test` delegates core portability checks to
pthread-enabled WebAssembly under Node; it does not validate the browser renderer or
full application deployment. Browser applications use `ENGINE_WEB_THREAD_CPPFLAGS`
and `ENGINE_WEB_THREAD_LDFLAGS` from `engine.mk` for a shared-memory Pthreads build,
OffscreenCanvas transfer, a two-worker startup pool, and Emscripten's Dawn WebGPU
port. The page must be cross-origin isolated (COOP same-origin and COEP
require-corp) for SharedArrayBuffer. `make webgpu-renderer-demo` builds an isolated
procedural renderer workload page using the existing `Renderer`; its localhost-only
server supplies those headers. For static hosting without configurable response
headers, the demo uses a same-origin service worker to set COOP/COEP before loading
its threaded WebAssembly module. The `Build and publish WebGPU demo` Pages workflow
is manual-only and does not enable Pages settings. See [performance demo
notes](PERFORMANCE.md) for its metrics, fixed resolution, publishing steps and the
existing 4,096-draw renderer limit. All linked C++ static libraries, including
Jolt, must be compiled with the same `-pthread` setting; `make jolt-wasm-pthread`
produces the matching Jolt archive.

`make web-worker-probe` builds an optional browser page that exercises the host's
real worker launch, worker-side WebGPU adapter/device creation, surface setup and
surface-texture acquisition, plus independent render and simulation pacing. Serve
the generated page over an isolated origin with WebGPU enabled. It is separate from
the native test suite and does not test game behavior. With Emscripten 6.0.10 and
headless Chromium on October 11, 2026, the probe verified both callbacks running
off the main thread, a configured OffscreenCanvas WebGPU surface, and independent
render/simulation counters.

The renderer currently uses WebGPU implicit resource transitions and has no explicit
backend barrier layer. GPU-driven culling, indirect drawing, GPU LOD, bindless-style
resource access, and mesh shaders are not implemented. A native host, cooked-asset
pipeline, persistent hash cache, hot reload, editor, and backend-neutral physics
interface are also future work, not current support claims.

Jolt Physics 5.6.0, cgltf 1.15, and stb 2.30/1.26 are pinned as source submodules.
The current stb_truetype checkout includes a repository-owned patch retained from
its previous vendored copy. Pins, patch verification, initialization, and license
provenance are documented in [dependency pins and patches](../dependencies/README.md).
Core's Xenium pin and portability patch are owned by the core repository.
