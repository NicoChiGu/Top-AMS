#!/usr/bin/env python3
"""Static acceptance checks for the A1 mini G-code and embedded web UI."""

from __future__ import annotations

import argparse
import importlib.util
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


# Importing the converter is part of validation; do not leave generated
# __pycache__ files in the source tree.
sys.dont_write_bytecode = True


REPO = Path(__file__).resolve().parents[3]
MAIN = REPO / "main"
DOC = REPO / "doc"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def ordered_positions(text: str, needles: list[str], context: str) -> None:
    cursor = -1
    for needle in needles:
        position = text.find(needle, cursor + 1)
        require(position >= 0, f"{context}: missing {needle!r}")
        require(position > cursor, f"{context}: {needle!r} is out of order")
        cursor = position


def validate_change_gcode() -> None:
    text = (DOC / "A1mini换料_topams版本.gcode").read_text(encoding="utf-8")
    pause = "M400 U1"
    require(text.count("M140 S{next_extruder + 1};EXT") == 1,
            "change G-code must contain exactly one ordinary channel marker")
    require(text.count(pause) == 1,
            "change G-code must contain exactly one pause point")

    pause_at = text.index(pause)
    ordered_positions(text[:pause_at], [
        "M140 S{next_extruder + 1};EXT",
        "G392 S0",
        "M1007 S0",
        "M204 S9000",
        "G1 Z{max_layer_z + 3.0} F1200",
        "G1 X180 F18000",
        "G1 Y90 F9000",
        "G1 X-13.5 F18000",
    ], "change G-code safety prelude")
    ordered_positions(text[pause_at:], [
        "M620 S[next_extruder]A",
        "T[next_extruder]",
        "M621 S[next_extruder]A",
        "M1007 S1",
    ], "change G-code tool-state restore")
    require("这些动作由 main.cpp 处理" not in text,
            "obsolete main.cpp delegation comment must be removed")


def validate_start_gcodes() -> None:
    official = (DOC / "A1mini启动_原版.gcode").read_text(encoding="utf-8")
    optional = (DOC / "A1mini启动_topams首料_20260513.gcode").read_text(
        encoding="utf-8"
    )
    insertion = (
        "M140 S{initial_no_support_extruder + 9};EXT_INIT\n"
        "M400 U1\n"
        "M140 S[bed_temperature_initial_layer_single]\n\n"
    )
    anchor = "M620 M ;enable remap"

    require(";===== date: 20260513" in official,
            "default startup template must be the official 20260513 revision")
    require("EXT_INIT" not in official and "M400 U1" not in official,
            "default official startup template must not auto-load first filament")
    require(official.count(anchor) == 1, "official startup M620 M anchor is ambiguous")
    require(optional == official.replace(anchor, insertion + anchor, 1),
            "optional startup must differ from official only by the three-line handshake")
    require(optional.count("EXT_INIT") == 1 and optional.count("M400 U1") == 1,
            "optional startup must contain exactly one initial-load marker and pause")
    require(optional.index("EXT_INIT") < optional.index(anchor),
            "initial-load handshake must be immediately before the original M620 M block")
    require("M620.3 W1" in official and "M620.3 W1" in optional,
            "filament tangle detection must remain enabled in both startup templates")


def load_converter():
    converter_path = MAIN / "convert_html_to_hpp.py"
    spec = importlib.util.spec_from_file_location("convert_html_to_hpp", converter_path)
    require(spec is not None and spec.loader is not None, "cannot load HTML converter")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def extract_script(html: str) -> str:
    matches = re.findall(r"<script[^>]*>(.*?)</script>", html,
                         flags=re.IGNORECASE | re.DOTALL)
    require(len(matches) == 1, "embedded UI must have exactly one inline script")
    return matches[0]


def node_check(script: str, node: str) -> None:
    with tempfile.TemporaryDirectory(prefix="top-ams-web-") as temporary:
        js_path = Path(temporary) / "index.js"
        js_path.write_text(script, encoding="utf-8")
        result = subprocess.run(
            [node, "--check", str(js_path)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        require(result.returncode == 0,
                "embedded JavaScript syntax failed:\n" + result.stdout + result.stderr)


def validate_web(node: str) -> None:
    source = (MAIN / "index.html").read_text(encoding="utf-8")
    for required in (
        'id="printer-operation-banner"',
        'aria-live="assertive"',
        "printer_operation_status",
        "formatAmsAction",
        "applyPrinterStageHighlight",
        "channel_confirmation_required",
        "电机已停止，打印保持暂停",
        "等待打印机状态",
    ):
        require(required in source, f"web UI is missing {required!r}")
    require("currentExtruderEl.textContent = '通道 1'" not in source,
            "web UI must not pretend channel 1 is loaded")
    require("@media (max-width: 768px)" in source,
            "web UI must retain its narrow-screen layout")

    node_check(extract_script(source.replace("CHANNEL_COUNT_PLACEHOLDER", "8")), node)

    converter = load_converter()
    with tempfile.TemporaryDirectory(prefix="top-ams-hpp-") as temporary:
        temporary_path = Path(temporary)
        for channels in (4, 6, 8):
            output = temporary_path / f"index-{channels}.hpp"
            converter.convert_html_to_hpp(
                str(MAIN / "index.html"),
                str(output),
                '#pragma once\n#include <string_view>\n\nconstexpr std::string_view web = R"rawliteral(',
                ')rawliteral";',
                channels,
            )
            generated = output.read_text(encoding="utf-8")
            require("CHANNEL_COUNT_PLACEHOLDER" not in generated,
                    f"{channels}-channel resource still has a placeholder")
            require(f"const CHANNEL_COUNT = {channels};" in generated,
                    f"{channels}-channel resource has the wrong channel count")
            raw_start = generated.index('R"rawliteral(') + len('R"rawliteral(')
            raw_end = generated.rindex(')rawliteral";')
            node_check(extract_script(generated[raw_start:raw_end]), node)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--node", default=shutil.which("node") or "node")
    args = parser.parse_args()

    validate_change_gcode()
    validate_start_gcodes()
    validate_web(args.node)
    print("G-code and web resource validation passed for 4/6/8 channels")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
