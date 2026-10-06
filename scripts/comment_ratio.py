#!/usr/bin/env python3
"""comment_ratio.py —— 产品源码注释率统计（只读、确定性、仅标准库）。

统计口径（与课程评分表的口径一致，且不依赖任何外部脚本）：

    COMMENT_RATIO = COMMENT_ONLY_LINES / TOTAL_PHYSICAL_LINES

  * TOTAL_PHYSICAL_LINES：统计范围内所有产品源码文件的物理行数（含空行、
    含注释行）；
  * COMMENT_ONLY_LINES：该行的主要内容属于注释 —— 行首是 // 或 /*，或者
    上一行开启了块注释且本行仍在块注释里；
  * INLINE_COMMENT_LINES：代码后面跟了行尾注释的行。它们**不计入**
    COMMENT_ONLY_LINES，避免用行尾注释去冲比例。

为什么需要一个真正的扫描器而不是 line.startswith("//")：字符串字面量里的
"//" 与 "/*" 会被朴素实现误判成注释。这里维护一个最小词法状态机：

    NORMAL / STRING / CHAR / LINE_COMMENT / BLOCK_COMMENT / RAW_STRING

覆盖 C++ 的三种字面量（"..."、'...'、R"delim(...)delim"）与两种注释，QML 沿用
同一套规则（QML 没有原始字符串与转义差异，走同一分支即可）。它不是完整
C++ 语法分析器，但足以保证"注释行"的判定不受字符串内容影响。

用法：
    python3 scripts/comment_ratio.py            # 人类可读输出
    python3 scripts/comment_ratio.py --json     # 机器可读输出
    python3 scripts/comment_ratio.py --self-test  # 跑内置 fixture 自检
"""

import argparse
import json
import os
import sys

SOURCE_ROOTS = ["app", "src", "include", "server", "tools", "ui/desktop", "ui/modern"]

SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".qml"}

EXCLUDED_DIR_MARKERS = ("third_party", "vendor", "generated", "build/", "tests/")


def counted_files(root):
    """\u8fd4\u56de\u7edf\u8ba1\u8303\u56f4\u5185\u7684\u6587\u4ef6\uff08\u8def\u5f84\u76f8\u5bf9\u4e8e\u4ed3\u5e93\u6839\uff0c\u5df2\u6392\u5e8f\uff09\u3002"""
    found = []
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
                absolute = os.path.join(dirpath, name)
                relative = os.path.relpath(absolute, root).replace(os.sep, "/")
                if any(marker in relative for marker in EXCLUDED_DIR_MARKERS):
                    continue
                # \u7b2c\u4e09\u65b9\u5934\u6587\u4ef6\uff08\u4f8b\u5982 vendored sqlite3.h\uff09\u4e0d\u5c5e\u4e8e\u672c\u9879\u76ee\u624b\u5199\u4ee3\u7801\u3002
                if name == "sqlite3.h":
                    continue
                found.append(relative)
    return sorted(found)


def scan_file(path):
    """\u626b\u63cf\u5355\u4e2a\u6587\u4ef6\uff0c\u8fd4\u56de (total, blank, comment_only, inline)\u3002"""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()          # \u6587\u4ef6\u672b\u5c3e\u7684\u6362\u884c\u4e0d\u7b97\u4e00\u884c

    total = len(lines)
    blank = 0
    comment_only = 0
    inline = 0

    in_block = False
    for line in lines:
        stripped = line.strip()
        if stripped == "":
            blank += 1
            continue
        classified = classify_line(line, in_block)
        in_block = classified["ends_in_block"]
        if classified["comment_only"]:
            comment_only += 1
        elif classified["has_inline"]:
            inline += 1
    return total, blank, comment_only, inline


def classify_line(line, starts_in_block):
    """\u5224\u5b9a\u4e00\u884c\u662f\u4e0d\u662f\u201c\u7eaf\u6ce8\u91ca\u884c\u201d\uff0c\u4ee5\u53ca\u5b83\u662f\u5426\u5728\u5757\u6ce8\u91ca\u91cc\u7ed3\u675f\u3002

    \u72b6\u6001\uff1anormal / string / char / line_comment / block_comment / raw_string
    \u201c\u7eaf\u6ce8\u91ca\u884c\u201d\u7684\u5b9a\u4e49\uff1a\u8be5\u884c\u5728\u4efb\u4f55\u4ee3\u7801\u5b57\u7b26\u51fa\u73b0\u4e4b\u524d\u5c31\u5df2\u7ecf\u8fdb\u5165\u6ce8\u91ca\u72b6\u6001\uff0c\u4e14\u4e4b\u540e
    \u4e0d\u518d\u56de\u5230\u4ee3\u7801\u3002\u53ea\u8981\u8be5\u884c\u51fa\u73b0\u8fc7\u4ee3\u7801\u5b57\u7b26\uff0c\u5b83\u5c31\u53ea\u80fd\u7b97\u201c\u6709\u884c\u5c3e\u6ce8\u91ca\u7684\u4ee3\u7801\u884c\u201d\u3002
    """
    state = "block_comment" if starts_in_block else "normal"
    saw_code = False
    raw_terminator = None

    index = 0
    length = len(line)
    while index < length:
        ch = line[index]
        nxt = line[index + 1] if index + 1 < length else ""

        if state == "line_comment":
            break
        if state == "block_comment":
            if ch == "*" and nxt == "/":
                state = "normal"
                index += 2
                continue
            index += 1
            continue
        if state == "string":
            if ch == "\\":
                index += 2
                continue
            if ch == '"':
                state = "normal"
            index += 1
            continue
        if state == "char":
            if ch == "\\":
                index += 2
                continue
            if ch == "'":
                state = "normal"
            index += 1
            continue
        if state == "raw_string":
            if line.startswith(raw_terminator, index):
                index += len(raw_terminator)
                state = "normal"
                raw_terminator = None
                continue
            index += 1
            continue

        # ---- state == normal ----
        if ch == "/" and nxt == "/":
            state = "line_comment"
            break
        if ch == "/" and nxt == "*":
            state = "block_comment"
            index += 2
            continue
        if ch == "R" and nxt == '"':
            terminator = line.find("(", index + 2)
            if terminator != -1:
                delim = line[index + 2:terminator]
                raw_terminator = ")" + delim + '"'
                index = terminator + 1
                state = "raw_string"
                continue
        if ch == '"':
            state = "string"
            saw_code = True
            index += 1
            continue
        if ch == "'":
            state = "char"
            saw_code = True
            index += 1
            continue
        if not ch.isspace():
            saw_code = True
        index += 1

    comment_only = (not saw_code)
    has_inline = saw_code and (state in ("line_comment", "block_comment"))
    return {
        "comment_only": comment_only,
        "has_inline": has_inline,
        "ends_in_block": state == "block_comment",
    }


def summarize(root):
    files = counted_files(root)
    totals = {"files": 0, "total": 0, "blank": 0, "comment_only": 0, "inline": 0}
    per_file = []
    for relative in files:
        total, blank, comment_only, inline = scan_file(os.path.join(root, relative))
        totals["files"] += 1
        totals["total"] += total
        totals["blank"] += blank
        totals["comment_only"] += comment_only
        totals["inline"] += inline
        per_file.append({"file": relative, "total": total,
                         "comment_only": comment_only, "inline": inline})
    ratio = (100.0 * totals["comment_only"] / totals["total"]
             if totals["total"] else 0.0)
    return totals, ratio, per_file


def self_test():
    """\u5c0f\u578b fixture\uff1a\u786e\u8ba4\u5b57\u7b26\u4e32\u91cc\u7684 // \u4e0e /* \u4e0d\u4f1a\u88ab\u5f53\u6210\u6ce8\u91ca\u3002"""
    cases = [
        ("// only comment", True, False),
        ("/* block */", True, False),
        ("code();  // tail", False, True),
        ('std::string url = "https://example.com";', False, False),
        ('std::string x = "/* not a comment */";', False, False),
        ("char c = '/';", False, False),
        ('auto s = R"(a // b /* c */)";', False, False),
        ("int x = 1;", False, False),
    ]
    failures = 0
    for line, want_comment, want_inline in cases:
        got = classify_line(line, False)
        if got["comment_only"] != want_comment or got["has_inline"] != want_inline:
            failures += 1
            print("SELF-TEST FAIL: %r -> %r" % (line, got))
    # \u5757\u6ce8\u91ca\u8de8\u884c\uff1a\u7b2c\u4e00\u884c\u5f00\u542f\uff0c\u7b2c\u4e8c\u884c\u4ecd\u5728\u5757\u91cc\u3002
    first = classify_line("/* start", False)
    second = classify_line("   still inside */", first["ends_in_block"])
    if not (first["ends_in_block"] and second["comment_only"]):
        failures += 1
        print("SELF-TEST FAIL: multi-line block comment")
    print("SELF-TEST: %s (%d checks)" % ("PASS" if failures == 0 else "FAIL",
                                        len(cases) + 2))
    return 0 if failures == 0 else 1


def main():
    parser = argparse.ArgumentParser(description="product source comment ratio")
    parser.add_argument("--json", action="store_true", help="machine readable")
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in scanner fixtures")
    parser.add_argument("--root", default=None, help="repository root")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    totals, ratio, per_file = summarize(root)

    if args.json:
        print(json.dumps({"counted_paths": SOURCE_ROOTS,
                          "excluded_paths": ["tests", "scripts", "docs",
                                             "packaging", "third_party",
                                             "vendor", "generated", "build"],
                          "totals": totals, "ratio": ratio,
                          "files": per_file}, indent=2, sort_keys=True))
        return 0

    print("counted paths  : " + ", ".join(SOURCE_ROOTS))
    print("excluded paths : tests, scripts, docs, packaging, third_party, "
          "vendor, generated, build")
    print("FILES_COUNTED=%d" % totals["files"])
    print("TOTAL_PHYSICAL_LINES=%d" % totals["total"])
    print("BLANK_LINES=%d" % totals["blank"])
    print("COMMENT_ONLY_LINES=%d" % totals["comment_only"])
    print("INLINE_COMMENT_LINES=%d" % totals["inline"])
    print("COMMENT_RATIO=%.2f%%" % ratio)
    return 0


if __name__ == "__main__":
    sys.exit(main())