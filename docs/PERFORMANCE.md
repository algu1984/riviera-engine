# WebGPU renderer workload demo

The isolated browser demo exercises Riviera Engine's existing `engine::Renderer` through the normal WebGPU surface path, with its forward PBR materials, shadow pass, and MSAA resolve. The C++ simulation worker publishes generated transforms through `RequestedExchange`; the render worker reuses the extracted `Draw` vector and the fixed-step clock paces simulation at 120 Hz. The demo uses generated cube/sphere geometry only and makes no network requests for scene assets.

## Run

Build from the Engine checkout with `make webgpu-renderer-demo`, then start `python3 examples/webgpu-renderer-demo/serve.py` and open `http://127.0.0.1:8788`. The server binds only to loopback and adds COOP/COEP headers required by shared-memory WebAssembly threads. Use the object selector for deterministic 100, 1,000, and 4,000 draw-record cases. The camera angle slider changes the orbit without changing the workload. The canvas is rendered at its fixed 1024 × 640 drawing-buffer resolution.

## Publish to GitHub Pages manually

The `Build and publish WebGPU demo` workflow runs only when someone chooses **Run workflow** in the repository's Actions tab. It builds the demo with Emscripten 6.0.10, checks out a pinned Riviera Engine Core revision, initializes only the pinned cgltf and stb submodules, copies the Core native sample, and deploys the generated static files. It does not run on pushes, pull requests, or a schedule. The published [benchmark results page](../examples/webgpu-renderer-demo/benchmarks.html) presents only Riviera Engine Core ECS, scheduler, and RequestedExchange measurements, with source instructions and downloadable raw JSON.

The published pages are the [WebGPU demo](https://algu1984.github.io/riviera-engine/) and [native Core measurements](https://algu1984.github.io/riviera-engine/benchmarks.html).

Before the first run, open **Settings → Pages** and select **GitHub Actions** as the build and deployment source. Then choose **Actions → Build and publish WebGPU demo → Run workflow** and select the branch to build. The deploy job reports the resulting site URL. Each run replaces the previously published demo.

GitHub Pages does not provide the COOP/COEP headers required by `SharedArrayBuffer`. On the hosted page, a small same-origin service worker adds those headers to same-origin responses, claims the page, and triggers one controlled reload before loading the WebAssembly module. If the browser cannot establish isolation, the page shows an error and a retry button instead of repeatedly reloading. The local server already sends the headers, so the module starts directly without registering a service worker. To exercise the hosted setup locally, run `python3 examples/webgpu-renderer-demo/serve.py --without-isolation` and open `http://127.0.0.1:8789`.

## Read the metrics

- **Frame cadence** is elapsed wall time between render-worker callbacks. Its rolling p50 reciprocal is shown as FPS and is commonly capped by the browser's display refresh and FIFO presentation mode.
- **CPU renderer call** is wall-clock duration around `Renderer::render`. It includes CPU-side object-data preparation, buffer uploads, command encoding and submission. It is not a GPU timestamp and must not be read as GPU execution time.
- **Simulation step** measures one simulation pump, including fixed-step clock work and any requested extraction. Fixed steps are 1/120 s; the worker pump is scheduled every 2 ms.
- **Draw records** counts records in the latest published packet. One record reuses one of two uploaded meshes and one of four generated roughness/metallic materials. The existing renderer can accept at most 4,096 records per render call.
- **Packet age** is the age of the latest simulation packet when the render worker consumes it. **Draw extractions** counts produced packets.
- **Capacity growths** counts growth of the demo's reusable packet vectors after their initial reserve. It excludes allocations internal to `Renderer`; the current renderer creates temporary object-data and sorted-particle vectors during rendering.

Results depend on browser, GPU, driver, power mode and display refresh. Compare runs on the same machine, browser configuration and fixed resolution. Use **Download report** to capture the latest values and selected workload as JSON. The demo is a measurement aid, not a cross-device benchmark score.
