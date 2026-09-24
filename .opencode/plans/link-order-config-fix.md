# Fix Static Link Order via Protobuf CONFIG (1-Line Root-Cause Fix)

## Summary

Switch `CMakeLists.txt:14` from Module-mode `find_package(Protobuf)` to `find_package(Protobuf CONFIG)`, then clean-rebuild, so CMake orders `/usr/local/lib/libprotobuf.a` before the `absl` archives it needs and `pmlmq_server`, `pmlmq_integration_tests`, and `pmlmq_demo` link. Keeps the modern `/usr/local` trio (protoc 31.1 / gRPC 1.79-dev / absl `lts_20250512`); no installs, no downgrade.

## Context / Findings

- Precise root cause (fresh exploration): single-pass static `ld` fails because Module-mode `protobuf::libprotobuf` is a bare `/usr/local/lib/libprotobuf.a` path with no `INTERFACE` deps, while CONFIG `gRPC::grpc++` emits `libabsl_*.a` first — so `link.txt` lands `libprotobuf.a` dead last (position 111 of 112), after the absl archives that satisfy its `cord_internal::GetEstimated*MemoryUsage` refs.
- Rich CONFIG target exists and is correct: `/usr/local/lib/cmake/protobuf/protobuf-targets.cmake` defines `protobuf::libprotobuf STATIC IMPORTED` with ~35 `absl::*` + `utf8_range` in `INTERFACE_LINK_LIBRARIES`; its config auto-finds `absl`/`utf8_range`.
- `pmlmq_proto` is `STATIC` (`CMakeLists.txt:40-41`) with `PUBLIC gRPC::grpc++ protobuf::libprotobuf`, so one central fix propagates to all three failing final links (`CMakeLists.txt:64`, `demo/CMakeLists.txt:2`, `tests/CMakeLists.txt:20-22`).
- `pmlmq_unit_tests` links only because `--as-needed` drops the unreferenced `grpc.pb.o` members needing `Cord`; server/demo/integration instantiate `ServerBuilder/Service/Channel/Message` and force full resolution.
- Must-not-break (service layer): `route_message(Message)` signature/sink wiring, `original_priority` reset on Nack re-queue (`src/pmlmq_service.cpp:210-211`), `enqueue_time` reset on enqueue/promotion, TTL-from-`arrival_time` with `0`=off, in-flight ownership map + `PERMISSION_DENIED`/`NOT_FOUND` contract, `Pull` wait-cap/poll behavior, `<ns>-<counter>` IDs, `__producer_id` header, proto package `pmlmq_rpc` vs namespace `pmlmq`. Keep `processing_time_ms` fields (Phase 2 hook) even though Ack/Nack ignore them today.
- Rejected alternatives: explicit absl re-list / `--start-group` (fragile hand-maintained list, GNU-ld-only, rots on upgrade — fallback only for CONFIG-less envs); apt shared-lib switch (needs `libgrpc++-dev` install, downgrades codegen 31.1→3.21 and gRPC 1.79→1.51).

## Approach

One-line root-cause fix: `find_package(Protobuf CONFIG REQUIRED)` makes `protobuf::libprotobuf` the rich IMPORTED target, so CMake topologically sorts `libprotobuf.a` before its absl deps in every final link. Clean rebuild required (stale Module-mode `Protobuf_LIBRARY_*` cache vars linger otherwise). Chosen over workarounds and toolchain switches per fresh evidence + user confirmation.

## Files affected

- `CMakeLists.txt:14` — `find_package(Protobuf REQUIRED)` → `find_package(Protobuf CONFIG REQUIRED)` (optionally + `find_package(absl CONFIG)` / `utf8_range` hardening; update stale apt-install comment at `:11` if touched).
- `build/` — deleted and regenerated (clean rebuild); no other source changes expected.
- `README.md` — only if build notes need a CONFIG/cache-wipe note (docs touch, minor).

## Step-by-step implementation

1. Apply the 1-line edit at `CMakeLists.txt:14`; leave `:30` (`protobuf::protoc`), `:41`, `:64`, `demo/CMakeLists.txt:2`, `tests/CMakeLists.txt:20-22` untouched.
2. Sanitize shell: `conda deactivate` (or PATH without conda) so no 29.3 `protoc` leaks in; verify `which -a protoc` lists `/usr/local/bin/protoc` first.
3. `rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local && cmake --build build -j$(nproc)`.
4. Verify link order: `build/CMakeFiles/pmlmq_server.dir/link.txt` shows `libprotobuf.a` before `libabsl_cord.a`/absl set; no `undefined reference to absl::...Cord` errors.
5. Run `./build/tests/pmlmq_unit_tests` (31/31), `./build/tests/pmlmq_integration_tests` (26 TEST_F), `./build/demo/pmlmq_demo` (expect DLQ ≈ 0, retries > 0).
6. If CONFIG ever unavailable on another machine, fall back to central absl re-list on `pmlmq_proto` (never global `--start-group`).

## Risks / edge cases

- Stale cache: reusing existing `build/` without wipe keeps Module-mode `Protobuf_LIBRARY_RELEASE` → fix silently not applied. Mitigation is mandatory `rm -rf build`.
- CONFIG hard-requires `/usr/local/lib/cmake/protobuf/` files; fails on Module-only images (not the case here, but reduces portability — document the requirement).
- CONFIG enforces triplet version match, surfacing previously-silent skew as configure errors (desirable but a new failure mode on mismatched systems).
- Conda reactivation re-shadows `protoc` 29.3 vs 31.1 headers; keep the PATH note.

## Testing strategy

- Clean rebuild exits 0 with no `absl::Cord` undefined refs across all three previously-failing targets.
- `ctest --test-dir build` green; unit 31/31, integration 26/26; `pmlmq_demo` end-to-end (2 producers, 2 consumers, NACK→retry visible).
- `link.txt` audit: `libprotobuf.a` precedes absl archives; `ldd` shows `/usr/local` static set consistently, no apt/conda mixing.

## Open questions

- None blocking. Note: `TTL_EXPIRED` has zero end-to-end coverage (unit container ops only) and demo never forces DLQ — flagged as follow-up test work, out of scope for this link fix.
