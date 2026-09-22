#!/usr/bin/env python3
# Generates src/sensecap/station_config_profile.h from one entry of
# data/profiles/profiles*.json (see data/profiles/README.md for the
# schema and webflash/README.md for the full flash-a-station workflow
# this feeds into).
#
# The committed, at-rest state of the generated header is always the
# built-in "dev" profile below -- the same values station_config.cpp used
# to hardcode -- so a fresh checkout builds unchanged without picking a
# profile first, and the Ersteinrichtungs-Assistent still runs like today.
# scripts/build_profile_firmware.py regenerates this file per profile
# before building, then restores "dev" afterwards.
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PROFILES_JSON = ROOT / "data" / "profiles" / "profiles.example.json"
OUT_HEADER = ROOT / "src" / "sensecap" / "station_config_profile.h"

DEV_PROFILE = {
    "name": "dev",
    "stationId": "STATION-01",
    "operatorName": "Unbekannter Betreiber",
    "localContact": "Kein lokaler Kontakt hinterlegt",
    "settingsPin": "1234",
    "dispatchNodeNum": None,
    "locationText": "",
}


def c_string(s: str) -> str:
    escaped = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def load_profile(name: str, profiles_file: Path) -> dict:
    if name == "dev":
        return DEV_PROFILE
    data = json.loads(profiles_file.read_text(encoding="utf-8"))
    for p in data["profiles"]:
        if p["name"] == name:
            return p
    known = ", ".join(p["name"] for p in data["profiles"])
    print(f"error: profile '{name}' not found in {profiles_file} (known: dev, {known})", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("profile", help="Profile name from --profiles-file, or 'dev' for the built-in default")
    ap.add_argument("--profiles-file", type=Path, default=DEFAULT_PROFILES_JSON)
    args = ap.parse_args()

    profile = load_profile(args.profile, args.profiles_file)

    dispatch = profile.get("dispatchNodeNum")
    has_dispatch = dispatch is not None
    # base=0 lets "0xce0ffa28" (hex, as node IDs are normally written) and
    # plain decimal strings both work.
    dispatch_literal = f"{int(str(dispatch), 0)}UL" if has_dispatch else "0UL"

    lines = [
        "#pragma once",
        "// GENERATED FILE -- do not edit by hand.",
        f"// Aktives Profil: {profile['name']}",
        "// Regenerate: python3 scripts/generate_profile_header.py <profile-name>",
        "// (oder ueber scripts/build_profile_firmware.py, das dies automatisch tut)",
        "",
        f"#define PROFILE_NAME {c_string(profile['name'])}",
        f"#define PROFILE_STATION_ID {c_string(profile['stationId'])}",
        f"#define PROFILE_OPERATOR_NAME {c_string(profile['operatorName'])}",
        f"#define PROFILE_LOCAL_CONTACT {c_string(profile['localContact'])}",
        f"#define PROFILE_SETTINGS_PIN {c_string(profile['settingsPin'])}",
        f"#define PROFILE_HAS_DISPATCH {1 if has_dispatch else 0}",
        f"#define PROFILE_DISPATCH_NODE_NUM {dispatch_literal}",
        f"#define PROFILE_LOCATION_TEXT {c_string(profile.get('locationText') or '')}",
        "",
    ]

    OUT_HEADER.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT_HEADER} (profile: {profile['name']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
