"""Compress the single web source into the firmware's C++ header resource."""

from __future__ import annotations

import argparse
import os
from pathlib import Path


def convert_html_to_hpp(
    input_file: str,
    output_file: str,
    header_str: str = "",
    footer_str: str = "",
    channel_count: int = 8,
) -> None:
    if channel_count not in (4, 6, 8):
        raise ValueError(f"通道数必须是 4、6 或 8，实际为 {channel_count}")

    input_path = Path(input_file)
    output_path = Path(output_file)
    processed_lines: list[str] = []
    for line in input_path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("//"):
            continue
        processed_lines.append(stripped)

    compressed_html = "".join(processed_lines)
    placeholder = "CHANNEL_COUNT_PLACEHOLDER"
    if compressed_html.count(placeholder) != 1:
        raise ValueError("index.html 必须恰好包含一个通道数占位符")
    compressed_html = compressed_html.replace(placeholder, str(channel_count))

    output_path.write_text(
        f"{header_str}\n{compressed_html}\n{footer_str}", encoding="utf-8"
    )
    # Keep build output ASCII-only: ESP-IDF can run under a GBK console on
    # Windows, where forwarding arbitrary Unicode text may abort idf.py.
    print(f"Generated {output_path} ({channel_count} channels)")


def main() -> int:
    env_channel_count = int(os.environ.get("CHANNEL_COUNT", "8"))
    parser = argparse.ArgumentParser(description="将 index.html 转为固件 index.hpp")
    parser.add_argument("-i", "--input", default="index.html", help="输入 HTML")
    parser.add_argument("-o", "--output", default="index.hpp", help="输出 HPP")
    parser.add_argument(
        "--header",
        default=(
            "#pragma once\n"
            "#include <string_view>\n\n"
            'constexpr std::string_view web = R"rawliteral('
        ),
        help="HPP 头部文本",
    )
    parser.add_argument(
        "--footer", default=")rawliteral\";", help="HPP 尾部文本"
    )
    parser.add_argument(
        "--channels",
        type=int,
        default=env_channel_count,
        choices=(4, 6, 8),
        help=f"通道数（默认读取 CHANNEL_COUNT，当前为 {env_channel_count}）",
    )
    args = parser.parse_args()
    convert_html_to_hpp(
        input_file=args.input,
        output_file=args.output,
        header_str=args.header,
        footer_str=args.footer,
        channel_count=args.channels,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
