# Browser workload demo measurements

The browser demo exercises five modes through one shared C++ executor and the
Engine WebGPU renderer: a procedural renderer scene, ECS hierarchy transforms,
independent ECS job systems, Jolt rigid-body collisions, and a 7-by-5
metallic/roughness material grid. The PBR grid reuses the renderer's existing
material and lighting path; it does not add image-based lighting or a new
shader. All scene simulation, workload scheduling, physics, and draw extraction
run in C++. JavaScript handles browser startup, form controls, report download,
and display of values exported by C++.

## Run and configure

Build with `make webgpu-renderer-demo`, then start
`python3 examples/webgpu-renderer-demo/serve.py` and open
`http://127.0.0.1:8788`. The server binds only to loopback and adds the COOP/COEP
headers needed by shared-memory WebAssembly threads. The hosted page uses a
same-origin service worker to establish browser isolation. The page includes a
bounded reload and a retry message if the worker cannot take control.

Choose Renderer counts of 100, 1,000, or 4,000. ECS hierarchy and Job systems
simulate 1,000, 10,000, or 100,000 entities while extracting at most 4,096 draw
records. Jolt physics supports 100, 500, 1,000, or 2,000 moving bodies and also
renders the static floor. Bodies settle under gravity, so active body load
naturally decreases as collisions resolve. The PBR grid always contains 35
spheres: roughness varies across seven columns and metallic response across five
rows. Its legend states the axis directions. The camera angle control changes
only the view.

The helper selector accepts `auto`, `0`, `2`, `4`, or `8` helpers through the
`helpers` URL parameter and reloads the page when changed. `auto` uses the
browser hardware concurrency value, or eight when it is missing, and reserves
two threads for rendering and simulation. Explicit helper selections are capped
at `max(hardwareConcurrency - 2, 0)`. The metric reports the number that the
executor actually started. To compare helper settings, reload the page between
runs and use the same browser, device, and selected workload.

## Read the measurements

- **Frame cadence** is elapsed time between render callbacks; the displayed
  reciprocal is a cadence estimate, not a simulation-rate guarantee.
- **CPU renderer call** measures CPU wall time around `Renderer::render`,
  including preparation, buffer uploads, command encoding, and submission. It
  is not GPU execution time.
- **Simulation pump CPU** measures the whole simulation worker pump. **Workload
  step** and **extraction** are reported separately where the workload provides
  them. The ECS/jobs modes can take longer than the fixed 1/120-second step at
  high counts; the UI reports their measured step duration and does not imply
  they are keeping up with real time.
- **Simulated entities** is the workload's active population. **Draw records**
  is the number extracted into the latest snapshot; entity counts can exceed
  the rendering limit.
- **Systems and tasks**, **helper execution**, **peak concurrency**, and the
  **checksum** describe the latest workload step where those values are
  available. Physics reports body state and helper execution; peak task
  concurrency is only meaningful for ECS and job workloads.
- **Packet age** is the time since the latest snapshot was extracted when the
  render worker consumed it. The snapshot carries its own mode and configuration
  generation so displayed workload values stay tied to the packet currently rendered.
- **Draw buffer growths** counts capacity changes in the reusable snapshot
  vector. It excludes allocations inside the renderer, ECS, scheduler, or Jolt.
- **Discarded clock time** indicates elapsed time the fixed-step clock could not
  accept after delays; it helps identify when simulation falls behind.

Use **Download report** to save the displayed values and configuration as JSON.
The report contains current browser measurements, not a cross-device score. The
separate [Core benchmark results](../examples/webgpu-renderer-demo/benchmarks.html)
page retains its original Core-only native measurements and methodology.
