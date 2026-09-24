# Fix the Link Breakage (Apt Shared-Lib Toolchain)

## Summary

Pin the build to a single Ubuntu apt toolchain (shared libs) and reconfigure clean, so `pmlmq_server`, `pmlmq_integration_tests`, and `pmlmq_demo` link instead of failing on `absl::Cord` undefined refs.

## Context / Findings

- Three toolchains coexist: `/usr/local` source set (protoc 31.1 / gRPC 1.79-dev / absl `lts_20250512`, static `.a` only), apt `/usr` set (protoc 3.21.12 / `libprotobuf.so.32`, absl `debian9` shared, **no `libgrpc++-dev` installed**), conda set (protoc 29.3 / gRPC ~1.71, first in `PATH`).
- `CMakeLists.txt:14` uses `find_package(Protobuf)` in module mode (bare `libprotobuf.a`, no transitive deps) while `CMakeLists.txt:15` uses `find_package(gRPC CONFIG)` — the module target drops the `absl/cord/utf8_range/re2` transitives that CONFIG `protobuf::libprotobuf` carries.
- `pmlmq_proto` (`CMakeLists.txt:40-42`) propagates only `gRPC::grpc++ + protobuf::libprotobuf`; nothing links `absl/re2/c-ares/zlib/ssl` explicitly. Old shared builds hid this via `DT_NEEDED`; static archives expose it.
- `pmlmq_unit_tests` links only because `--as-needed` drops the unreferenced `grpc.pb.o` members (queue/DLQ/proxy tests use no gRPC symbols). Server/demo/integration instantiate `ServerBuilder/Service/Channel/Message` and force full resolution, so only they fail.
- `build/CMakeCache.txt` currently pins `/usr/local` (`Protobuf_LIBRARY_RELEASE=/usr/local/lib/libprotobuf.a`, `gRPC_DIR=/usr/local/lib/cmake/grpc`, protoc 31.1); `build/pmlmq_server`, `build/tests/pmlmq_integration_tests`, `build/demo/pmlmq_demo` binaries are absent (re-verified).
- Tradeoff considered: pinning the existing `/usr/local` static trio is the upstream-correct fix with no installs, but the decision is apt-shared per user choice — expect a protobuf/gRPC version downgrade and regenerated `pb.cc` differences.

## Approach

Single-vendor apt toolchain: install `libgrpc++-dev / libprotobuf-dev / protobuf-compiler-grpc` from apt, point CMake at the `/usr` CONFIG packages, sanitize `PATH` so conda protoc 29.3 no longer shadows, then clean-rebuild. Shared apt libs resolve `absl` transitively via `DT_NEEDED`, giving the smallest durable CMake diff.

## Files affected

- `CMakeLists.txt` — switch to `find_package(Protobuf CONFIG)` (+ matching `gRPC CONFIG`), set `CMAKE_PREFIX_PATH`/`<Pkg>_ROOT` to `/usr`, pin `protobuf::protoc` + `gRPC::grpc_cpp_plugin` program paths out of conda `PATH`.
- `README.md` — update deps/build notes to apt-only toolchain + conda-PATH warning.
- `build/` — deleted and regenerated (clean rebuild); no source `.cpp/.hpp/.proto` changes expected unless 3.21 codegen flags new warnings.

## Step-by-step implementation

1. Record baseline: `protoc --version`, `pkg-config --modversion protobuf grpc++`, `which -a protoc grpc_cpp_plugin`, `dpkg -l | grep -E 'protobuf|grpc|absl'`.
2. `sudo apt update && sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc` (adds the missing apt gRPC + plugin).
3. Deactivate conda / fix `PATH` so `protoc` resolves to apt 3.21, not conda 29.3; verify with `which protoc`.
4. CMake edits: `find_package(Protobuf CONFIG REQUIRED)`, keep `find_package(gRPC CONFIG REQUIRED)`, set `CMAKE_PREFIX_PATH=/usr`, ensure codegen uses the apt `protobuf::protoc` and grpc plugin.
5. `rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr && cmake --build build -j$(nproc)`.
6. Verify link closure: failing targets' `link.txt` reference `/usr/lib/...so`, no `/usr/local/...a` or `debian9` vs `lts_*` mix; `ldd build/pmlmq_server` shows apt `libprotobuf/absl/grpc` shared libs only.
7. Run `./build/tests/pmlmq_unit_tests`, `./build/tests/pmlmq_integration_tests`, `./build/demo/pmlmq_demo` (expect demo DLQ approx 0, retries > 0).
8. Update `README.md` deps section to apt-only + `PATH` note.

## Risks / edge cases

- Apt protobuf 3.21 vs code generated under 31.1: regeneration may surface deprecation errors or API diffs in `proto/pmlmq.proto` codegen.
- Apt gRPC version (likely <= 1.51) vs code using newer API: verify `ServerBuilder/RegisterService` calls still compile.
- If apt `libgrpc++-dev` pulls conflicting `libabsl`/`libssl` versions, links may still mix with `/usr/local` statics — mitigation is the `CMAKE_PREFIX_PATH=/usr` pin + `link.txt` audit.
- Conda reactivation re-shadows `protoc`; document the `PATH` requirement.
- Downgrade forfeits protobuf-31 features; fallback is the `/usr/local` CONFIG-pin approach (kept as backup).

## Testing strategy

- Full rebuild from clean exits 0 with no `absl::Cord` undefined refs.
- `ctest --test-dir build` / both test binaries pass; `pmlmq_demo` runs end-to-end (2 producers, 2 consumers, NACK-to-retry visible).
- `ldd` + `link.txt` audit: no `/usr/local/lib/*.a`, no mixed `debian9`/`lts_20250512` symbols.

## Open questions

- None blocking. Flag: if the 3.21 downgrade proves unacceptable (codegen/API friction in step 5), switch the pick back to the `/usr/local` static CONFIG pin.
