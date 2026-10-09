#!/usr/bin/env python3

import os
import re
import subprocess
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
SDK_DIR = next(parent.parent for parent in TEST_DIR.parents if parent.name == "ap")
AP_HEADER = SDK_DIR / "ap/include/driver/pwr_clk.h"
CP_HEADER = SDK_DIR / "cp/include/driver/pwr_clk.h"
BUILD_DIR = TEST_DIR / "build"
ARM_CC = os.environ.get(
    "ARM_CC",
    "/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc",
)
HOST_CC = os.environ.get("CC", "cc")
TYPE_NAME = "pm_shared_u64_words_t"
HELPERS = (
    "pm_shared_u64_load",
    "pm_shared_u64_store",
    "pm_shared_u64_bit_get",
    "pm_shared_u64_bit_set",
    "pm_shared_u64_bit_clear",
    "pm_shared_u64_is_zero",
)


def extract_struct(text):
    match = re.search(
        rf"typedef\s+struct(?:\s+\w+)?\s*\{{[\s\S]*?\}}\s*{TYPE_NAME}\s*;",
        text,
    )
    return match.group(0) if match is not None else ""


def normalize(text):
    return re.sub(r"\s+", " ", text).strip()


def extract_function(text, name):
    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{}}]*\)[^;{{}}]*\{{", text)
    if match is None:
        return ""
    start = text.rfind("static inline", 0, match.start())
    brace = text.find("{", match.start())
    depth = 0
    for position in range(brace, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[start : position + 1]
    return ""


def compile_probe(label, declaration, helpers):
    source = BUILD_DIR / f"pm05c_layout_{label}.c"
    output = BUILD_DIR / f"pm05c_layout_{label}.o"
    source.write_text(
        f"""
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
{declaration}
{helpers}
_Static_assert(sizeof({TYPE_NAME}) == 12U,
    "fixed64 seqlock must use version plus two words");
_Static_assert(offsetof({TYPE_NAME}, version) == 0U,
    "version must be first");
_Static_assert(offsetof({TYPE_NAME}, low) == 4U,
    "low word must follow version");
_Static_assert(offsetof({TYPE_NAME}, high) == 8U,
    "high word must follow low");
int pm05c_layout_probe(void) {{ return (int)sizeof({TYPE_NAME}); }}
""",
        encoding="utf-8",
    )
    return subprocess.run(
        [
            ARM_CC,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-mcpu=cortex-m33",
            "-mthumb",
            "-c",
            str(source),
            "-o",
            str(output),
        ],
        text=True,
        capture_output=True,
    )


def run_golden(label, declaration, helpers):
    source = BUILD_DIR / f"pm05c_layout_{label}_golden.c"
    output = BUILD_DIR / f"pm05c_layout_{label}_golden"
    source.write_text(
        f"""
#include <stdbool.h>
#include <stdint.h>
{declaration}
{helpers}
int main(void)
{{
    volatile {TYPE_NAME} field = {{0}};
    const uint64_t golden = UINT64_C(0x1122334455667788);
    if (!pm_shared_u64_store(&field, golden)
        || pm_shared_u64_load(&field) != golden) return 1;
    __atomic_store_n(&field.version, 1U, __ATOMIC_RELEASE);
    if (pm_shared_u64_load(&field) != UINT64_MAX) return 2;
    __atomic_store_n(&field.version, UINT32_MAX - 1U, __ATOMIC_RELEASE);
    if (pm_shared_u64_store(&field, golden)) return 3;
    __atomic_store_n(&field.version, 2U, __ATOMIC_RELEASE);
    __atomic_store_n(&field.low, UINT32_C(0x55667788), __ATOMIC_RELEASE);
    __atomic_store_n(&field.high, UINT32_C(0x11223344), __ATOMIC_RELEASE);
    const volatile unsigned char *bytes =
        (const volatile unsigned char *)&field;
    return (sizeof(field) == 12U
        && bytes[0] == 0x02U && bytes[4] == 0x88U
        && bytes[5] == 0x77U && bytes[6] == 0x66U
        && bytes[7] == 0x55U && bytes[8] == 0x44U
        && bytes[9] == 0x33U && bytes[10] == 0x22U
        && bytes[11] == 0x11U) ? 0 : 4;
}}
""",
        encoding="utf-8",
    )
    compile_result = subprocess.run(
        [
            HOST_CC,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(source),
            "-o",
            str(output),
        ],
        text=True,
        capture_output=True,
    )
    if compile_result.returncode != 0:
        return compile_result
    return subprocess.run([str(output)], text=True, capture_output=True)


def main():
    try:
        ap_text = AP_HEADER.read_text(encoding="utf-8")
        cp_text = CP_HEADER.read_text(encoding="utf-8")
    except OSError as error:
        print(f"PM-05C:LAYOUT:HARNESS-FAIL {error}", file=sys.stderr)
        return 2
    ap_struct = extract_struct(ap_text)
    cp_struct = extract_struct(cp_text)
    missing = []
    if not ap_struct:
        missing.append("ap-shared-fixed64-layout")
    if not cp_struct:
        missing.append("cp-shared-fixed64-layout")
    if missing:
        for item in missing:
            print(f"PM-05C:LAYOUT:MISSING {item}", file=sys.stderr)
        print(f"PM-05C:LAYOUT:FAIL compiled=0 missing={len(missing)}")
        return 1
    if normalize(ap_struct) != normalize(cp_struct):
        print(
            "PM-05C:LAYOUT:MISSING ap-cp-layout-byte-consistency",
            file=sys.stderr,
        )
        return 1
    if not all(
        token in normalize(ap_struct)
        for token in ("uint32_t version", "uint32_t low", "uint32_t high")
    ):
        print(
            "PM-05C:LAYOUT:MISSING version-low-high-seqlock-layout",
            file=sys.stderr,
        )
        return 1
    ap_helpers = "\n".join(extract_function(ap_text, name) for name in HELPERS)
    cp_helpers = "\n".join(extract_function(cp_text, name) for name in HELPERS)
    if not ap_helpers or normalize(ap_helpers) != normalize(cp_helpers):
        print("PM-05C:LAYOUT:MISSING ap-cp-helper-consistency", file=sys.stderr)
        return 1
    load = extract_function(ap_text, "pm_shared_u64_load")
    store = extract_function(ap_text, "pm_shared_u64_store")
    bit_set = extract_function(ap_text, "pm_shared_u64_bit_set")
    bit_clear = extract_function(ap_text, "pm_shared_u64_bit_clear")
    if not (
        load.find("field->high") < load.find("field->low")
        and store.find("field->low") < store.find("field->high")
    ):
        print("PM-05C:LAYOUT:MISSING ordered-low-high-helpers", file=sys.stderr)
        return 1
    if re.search(
        r"(?:__atomic_thread_fence\s*\(\s*__ATOMIC_ACQUIRE|__DMB\s*\(\s*\))",
        load,
    ) is None:
        missing.append("reader-acquire-fence-or-dmb")
    if any(
        re.search(r"\*word\s*(?:\|=|&=|\^=|=)", body) is not None
        for body in (bit_set, bit_clear)
    ):
        missing.append("payload-word-updates-are-atomic")
    if not (
        all(
            token in store
            for token in (
                "__atomic_compare_exchange_n",
                "UINT32_MAX - 2U",
                "return false",
            )
        )
        and re.search(
            r"retry\s*<\s*(?:PM_SHARED_U64_MAX_RETRIES|32U)", store
        )
        is not None
    ):
        missing.append("writer-cas-version-bounded-wrap-failclosed")

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    failures = 0
    for label, declaration in (("ap", ap_struct), ("cp", cp_struct)):
        helpers = ap_helpers if label == "ap" else cp_helpers
        result = compile_probe(label, declaration, helpers)
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            failures += 1
        golden_result = run_golden(label, declaration, helpers)
        if golden_result.returncode != 0:
            sys.stderr.write(golden_result.stdout)
            sys.stderr.write(golden_result.stderr)
            missing.append(f"{label}-odd-wrap-high-word-little-endian-golden")
    for item in missing:
        print(f"PM-05C:LAYOUT:MISSING {item}", file=sys.stderr)
    if failures or missing:
        print(
            f"PM-05C:LAYOUT:FAIL compiled={2 - failures} "
            f"missing={failures + len(missing)}"
        )
        return 1
    print(
        "PM-05C:LAYOUT:PASS compiled=2 words=3 "
        "consistent=1 ordered=1 golden=1"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
