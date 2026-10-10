# Engine dependencies

The engine keeps source dependencies as Git submodules under `third_party/`.
The parent repository's gitlink records each exact upstream commit; the
dependency tool verifies that checkout and applies repository-owned patches.
Run `make deps` to initialize the engine submodules and the sibling core's
submodules. Run `make deps-check` for an offline verification. Normal build and
test targets validate local checkouts but do not fetch them.

| Dependency | Upstream | Pin | Local changes |
| --- | --- | --- | --- |
| Jolt Physics 5.6.0 | [jrouwe/JoltPhysics](https://github.com/jrouwe/JoltPhysics) | `e77f175595e64cb44218cc9d9d56fc365ad0e36a` | None |
| cgltf 1.15 | [jkuhlmann/cgltf](https://github.com/jkuhlmann/cgltf) | `360db1a95480fe102ae9c69b27c5d101167ff5ba` | None |
| stb_image 2.30 and stb_truetype 1.26 | [nothings/stb](https://github.com/nothings/stb) | `013ac3beddff3dbffafd5177e7972067cd2b5083` | `patches/stb-truetype.patch` retains the existing vendored header bytes |

The stb patch is kept in this repository and applied only to the checked-out
submodule working tree. Do not commit patched dependency files inside a submodule.
The original copies of all three dependency trees matched these pins after applying
the documented stb patch. License text and attribution remain in
`third_party/licenses/THIRD_PARTY_NOTICES.txt` and in the upstream submodules.

`JOLT_ROOT`, `CGLTF_ROOT`, and `STB_ROOT` may point to alternate compatible source
directories for local builds. The repository lock and `make deps` always initialize
the in-tree submodule paths.
