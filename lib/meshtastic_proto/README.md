# Vendored: nanopb + Meshtastic protobuf definitions

Not hand-written. Two things, vendored verbatim so our packets are byte-for-byte
compatible with real Meshtastic firmware instead of a hand-rolled guess:

- `pb.h`, `pb_common.*`, `pb_encode.*`, `pb_decode.*` — the nanopb runtime,
  version **0.4.9.1**, fetched from
  `https://raw.githubusercontent.com/nanopb/nanopb/nanopb-0.4.9.1/`.
  This exact version because it's what `meshtastic/firmware`'s own
  `platformio.ini` pins (`nanopb/nanopb` tag `nanopb-0.4.9.1`) — the generated
  headers below check `PB_PROTO_HEADER_VERSION` at compile time and refuse to
  build against a mismatched runtime.
- `meshtastic/*.pb.h` / `*.pb.cpp` — the **already-generated** nanopb output
  for Meshtastic's `.proto` schema, fetched directly from
  `meshtastic/firmware`'s `src/mesh/generated/meshtastic/` on `master`
  (2026-09-18). Not regenerated from `.proto` sources ourselves — no protoc/
  nanopb generator toolchain in this build, and using firmware's own generated
  output removes any risk of a field-numbering or options mismatch.

`platformio.ini` sets `-DPB_ENABLE_MALLOC=1 -DPB_VALIDATE_UTF8=1` for the
`sensecap_indicator` env to match how upstream firmware itself builds these
files.

## Why this matters here

`src/sensecap/meshtastic_proto.cpp` only needs `meshtastic_Data` (portnum +
payload bytes) — that's the one message actually protobuf-encoded on the wire;
the 16-byte packet header (to/from/id/flags/channel/next_hop/relay_node) is
raw, not protobuf. But `mesh.pb.h` transitively includes config/module_config/
device_ui/telemetry/etc. headers (Meshtastic bundles its whole schema through
one generated tree), so the rest of this folder exists purely to make that one
`#include` compile — it isn't otherwise used.

## Updating

If Meshtastic's `.proto` schema changes (new fields on `Data`, etc.), re-fetch
the files listed above from a newer `meshtastic/firmware` tag/commit and drop
them in unchanged. Don't hand-edit anything in `meshtastic/*.pb.h/.pb.cpp`.
