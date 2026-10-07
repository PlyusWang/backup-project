#!/usr/bin/env python3
"""source_style_check.py —— 只读的行宽审计（显示宽度，不是字节数）。

为什么单独一个工具：Google C++ Style 的 80 列是针对**显示宽度**的，而中文在
等宽字体下占两列。用 len() 或 UTF-8 字节数判断会把"一行 40 个汉字"（80 列）
误判成"很短"，也会把"一行 80 个 ASCII"误判成刚好。这里按 Unicode East Asian
Width 计算：W / F 记 2 列，其余记 1 列（ambig 记 1 列，与常见终端一致）。

clang-format 仍然是 C++ 代码格式的 authority；这个工具只补 clang-format 管不到
的两块：注释文本与 QML。

允许的例外（不拆）：
  * 含 URL 的行（拆了就点不开）；
  * 含 32 位以上十六进制串的行（SHA-256 / 指纹，拆开就不再是可复制的字面量）；
  * 含 shell 命令或路径且拆开会改变语义的行；
  * 协议测试向量。

用法：
    python3 scripts/source_style_check.py
    python3 scripts/source_style_check.py --json
"""

import argparse
import json
import os
import re
import sys
import unicodedata

SOURCE_ROOTS = ["app", "src", "include", "server", "tools", "ui/desktop", "ui/modern"]
SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".qml"}
MAX_WIDTH = 80

URL_PATTERN = re.compile(r"https?://")
HEX_PATTERN = re.compile(r"\b[0-9a-fA-F]{32,}\b")
COMMAND_PATTERN = re.compile(r"(sudo |apt-get |systemctl |ssh |bash scripts/)")


def display_width(text):
    width = 0
    for ch in text:
        if unicodedata.east_asian_width(ch) in ("W", "F"):
            width += 2
        else:
            width += 1
    return width


def is_comment_line(stripped):
    return (stripped.startswith("//") or stripped.startswith("/*")
            or stripped.startswith("*") or stripped.startswith("*/"))


def is_comment_exception(text):
    """注释行的例外：只有 URL 与指纹/哈希。

    刻意**不**把 shell 命令关键字当作注释行的例外：一条中文散文注释里只要出现 "ssh " /
    "sudo " 就被静默放行，会让 COMMENT_ACTIONABLE_OVER_80=0 看起来比实际更干净。
    注释里的命令可以在词边界换行而不丢信息。
    """
    return bool(URL_PATTERN.search(text) or HEX_PATTERN.search(text))


def is_code_exception(text):
    """代码行的例外：URL、指纹/哈希，以及本身就是一条命令的行。"""
    return bool(URL_PATTERN.search(text) or HEX_PATTERN.search(text)
                or COMMAND_PATTERN.search(text))


def iter_files(root):
    for base in SOURCE_ROOTS:
        base_path = os.path.join(root, base)
        if not os.path.isdir(base_path):
            continue
        for dirpath, dirnames, filenames in os.walk(base_path):
            dirnames[:] = sorted(d for d in dirnames
                                 if d not in ("third_party", "vendor",
                                              "generated", "build", ".git"))
            for name in sorted(filenames):
                if os.path.splitext(name)[1] not in SOURCE_EXTENSIONS:
                    continue
                if name == "sqlite3.h":
                    continue
                absolute = os.path.join(dirpath, name)
                relative = os.path.relpath(absolute, root).replace(os.sep, "/")
                if "third_party/" in relative or "vendor/" in relative:
                    continue
                yield relative, absolute


def main():
    parser = argparse.ArgumentParser(description="source line width audit")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--root", default=None)
    args = parser.parse_args()
    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    comment_over = []
    code_over = []
    exceptions = []
    for relative, absolute in iter_files(root):
        with open(absolute, "r", encoding="utf-8", errors="replace") as handle:
            for number, line in enumerate(handle.read().split("\n"), start=1):
                width = display_width(line.rstrip("\r"))
                if width <= MAX_WIDTH:
                    continue
                where = "%s:%d" % (relative, number)
                if is_comment_line(line.strip()):
                    if is_comment_exception(line):
                        exceptions.append(where)
                        continue
                    comment_over.append(where)
                else:
                    if is_code_exception(line):
                        exceptions.append(where)
                        continue
                    code_over.append(where)

    if args.json:
        print(json.dumps({
            "max_width": MAX_WIDTH,
            "comment_over": comment_over,
            "code_over": code_over,
            "allowed_exceptions": exceptions,
        }, indent=2, sort_keys=True))
        return 1 if (comment_over or code_over) else 0

    print("max width (display columns) = %d" % MAX_WIDTH)
    print("COMMENT_ACTIONABLE_OVER_80=%d" % len(comment_over))
    print("CODE_ACTIONABLE_OVER_80=%d" % len(code_over))
    print("ALLOWED_OVER_80_EXCEPTIONS=%d" % len(exceptions))
    for item in comment_over[:40]:
        print("  comment>80: " + item)
    for item in code_over[:40]:
        print("  code>80:    " + item)
    return 1 if (comment_over or code_over) else 0


if __name__ == "__main__":
    sys.exit(main())
