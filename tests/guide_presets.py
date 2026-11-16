"""Check guide presets against a configured ifj26-opt binary (no dependencies)."""

import argparse
import json
import os
import re
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def value_ids(source):
    return re.findall(r"\bv(\d+)\b", re.sub(r"#.*", "", source))


def invoke(binary, source, optimize=False):
    command = [str(binary)] + ([] if optimize else ["--no-opt"])
    result = subprocess.run(command, input=source, capture_output=True, timeout=10)
    try:
        envelope = json.loads(result.stdout)
    except (ValueError, UnicodeError) as error:
        raise AssertionError(
            f"exit {result.returncode}, no JSON response; "
            f"stderr: {result.stderr.decode(errors='replace')[-500:]}"
        ) from error
    return result.returncode, envelope


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / "release" /
                        ("ifj26-opt.exe" if os.name == "nt" else "ifj26-opt"))
    parser.add_argument("--optimize", action="store_true",
                        help="also smoke-test optimization and reparse its output; not a semantic proof")
    args = parser.parse_args()
    binary = args.binary.resolve()
    failures = []
    presets = sorted((ROOT / "presets").glob("*_guide_*.ir"))
    assert len(presets) == 25, "guide coverage changed; update the inventory and checks"
    for step in range(1, 17):
        assert any(f"_guide_{step:02d}_" in p.name for p in presets), f"missing step {step}"

    for preset in presets:
        try:
            code, parsed = invoke(binary, preset.read_bytes())
            assert code == 0 and parsed.get("ok"), parsed
            assert value_ids(parsed["ir"]) == value_ids(preset.read_text()), "value ids changed"
            canonical = parsed["ir"].encode()
            code, reparsed = invoke(binary, canonical)
            assert code == 0 and reparsed.get("ok"), reparsed
            assert reparsed["ir"] == parsed["ir"], "unstable parser/printer round-trip"
            if args.optimize:
                code, optimized = invoke(binary, preset.read_bytes(), optimize=True)
                assert code == 0 and optimized.get("ok"), optimized
                code, checked = invoke(binary, optimized["ir"].encode())
                assert code == 0 and checked.get("ok"), checked
            print(f"PASS {preset.name}")
        except (AssertionError, subprocess.TimeoutExpired) as error:
            failures.append(preset.name)
            print(f"FAIL {preset.name}: {error}")

    # Descending ids must not shrink value_count or collide with later values.
    sparse = b""".func main : int
.block 0
v50 : int = call readInt
v7 : int = add v50, int 1
v42 : int = add v7, int 2
ret v42
"""
    code, parsed = invoke(binary, sparse)
    assert code == 0 and parsed.get("ok"), parsed
    assert value_ids(parsed["ir"]) == value_ids(sparse.decode()), parsed
    code, duplicate = invoke(binary, sparse.replace(b"v7 :", b"v50 :"))
    assert code != 0 and "defined twice" in duplicate.get("error", ""), duplicate

    invalid_cases = {
        "invalid_block_target.ir": "undefined block",
        "invalid_value.ir": "undefined value",
        "unknown_call.ir": "unknown function",
    }
    for name, message in invalid_cases.items():
        fixture = ROOT / "tests" / "fixtures" / "guide" / name
        try:
            code, result = invoke(binary, fixture.read_bytes())
            assert code != 0 and result.get("ok") is False, result
            assert message in result.get("error", ""), result
            print(f"PASS rejection: {name}")
        except (AssertionError, subprocess.TimeoutExpired) as error:
            failures.append(name)
            print(f"FAIL {name}: {error}")
    print(f"{len(presets)} guide presets, {len(invalid_cases)} negative fixtures, "
          f"{len(failures)} failures")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
