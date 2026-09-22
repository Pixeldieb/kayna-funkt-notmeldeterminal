#!/usr/bin/env python3
# Generates src/sensecap/districts_de.h from data/presets/districts_de.json --
# a compiled-in fallback so the Ort-Auswahl works without an SD card (see
# data/presets/README.md's "hybrid" design: SD card can later override this
# with a different locale/edition, but the firmware never depends on that
# working). Re-run this after editing districts_de.json; the header is
# committed like the other generated/vendored headers in this project
# (lib/meshtastic_proto/), not regenerated at build time.
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_JSON = ROOT / "data" / "presets" / "districts_de.json"
OUT_HEADER = ROOT / "src" / "sensecap" / "districts_de.h"


def c_string(s: str) -> str:
    escaped = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


# Workaround for issue #41: the bundled LVGL fonts (all sizes, see
# include/lv_conf.h) don't include Latin-1 Supplement, so ä/ö/ü/ß silently
# fail to render -- LVGL just skips the glyph rather than showing a visible
# replacement box, which is how this went unnoticed until now (see the
# issue for how this affects the rest of the UI too, e.g. "Zurück").
# German district/state names are transliterated ONLY here, in the
# generated-for-firmware header -- data/presets/districts_de.json (the
# source of truth, also used by anything else that reads it later) keeps
# correct German spelling. Drop this once #41 ships a proper font with
# Latin-1 Supplement.
_UMLAUT_MAP = str.maketrans({
    "ä": "ae", "ö": "oe", "ü": "ue", "ß": "ss",
    "Ä": "Ae", "Ö": "Oe", "Ü": "Ue",
})


def transliterate(s: str) -> str:
    return s.translate(_UMLAUT_MAP)


def main() -> int:
    data = json.loads(SRC_JSON.read_text(encoding="utf-8"))
    states = data["states"]
    districts = data["districts"]
    if data["count"] != len(districts):
        print(f"error: count field ({data['count']}) != len(districts) ({len(districts)})", file=sys.stderr)
        return 1

    state_index = {s["code"]: i for i, s in enumerate(states)}

    lines = []
    lines.append("// GENERATED FILE -- do not edit by hand.")
    lines.append(f"// Source: data/presets/districts_de.json (Stand {data['stand']})")
    lines.append("// Regenerate: python3 scripts/generate_districts_header.py")
    lines.append("#pragma once")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("struct BundeslandEntry {")
    lines.append("  const char *code;")
    lines.append("  const char *name;")
    lines.append("};")
    lines.append("")
    lines.append("struct DistrictEntry {")
    lines.append("  const char *name;")
    lines.append("  const char *kennzeichen;")
    lines.append("  uint8_t bundeslandIndex;")
    lines.append("};")
    lines.append("")
    lines.append(f"constexpr uint16_t kBundeslandCount = {len(states)};")
    lines.append("const BundeslandEntry kBundeslaender[kBundeslandCount] = {")
    for s in states:
        lines.append(f"  {{{c_string(s['code'])}, {c_string(transliterate(s['name']))}}},")
    lines.append("};")
    lines.append("")
    lines.append(f"constexpr uint16_t kDistrictCount = {len(districts)};")
    lines.append("const DistrictEntry kDistricts[kDistrictCount] = {")
    for d in districts:
        bl_idx = state_index[d["bundesland"]]
        lines.append(f"  {{{c_string(transliterate(d['name']))}, {c_string(d['kennzeichen'])}, {bl_idx}}},")
    lines.append("};")
    lines.append("")

    OUT_HEADER.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {OUT_HEADER} ({len(districts)} districts, {len(states)} states)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
