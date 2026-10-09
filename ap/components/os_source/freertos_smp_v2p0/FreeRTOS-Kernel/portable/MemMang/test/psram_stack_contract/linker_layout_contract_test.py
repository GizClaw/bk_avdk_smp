#!/usr/bin/env python3

from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
SDK_ROOT = TEST_DIR.parents[8]
LINKER = SDK_ROOT / "ap/middleware/soc/bk7258_ap/bk7258_ap_bsp.ld"
ARM_CC = Path("/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc")


def extract_stack_asserts(text: str) -> list[str]:
    statements: list[str] = []
    cursor = 0
    while True:
        start = text.find("ASSERT(", cursor)
        if start < 0:
            break
        depth = 0
        quote = False
        escaped = False
        end = start
        for end in range(start, len(text)):
            character = text[end]
            if quote:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == '"':
                    quote = False
                continue
            if character == '"':
                quote = True
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    break
        statement = text[start : end + 1]
        if "PSRAM_STACK" in statement:
            statements.append(statement)
        cursor = end + 1
    return statements


def missing_contracts(asserts: list[str]) -> list[str]:
    joined = "\n".join(asserts)
    requirements = {
        "nonzero-size": r"STACK_HEAP_SIZE[\s\S]*(?:!=|>)\s*0",
        "address-alignment": r"STACK_HEAP_ADDR[\s\S]*(?:31|32|ALIGN)",
        "size-alignment": r"STACK_HEAP_SIZE[\s\S]*(?:31|32|ALIGN)",
        "addition-overflow": (
            r"STACK_HEAP_ADDR[\s\S]*STACK_HEAP_SIZE[\s\S]*"
            r"(?:UINT32_MAX|STACK_HEAP_ADDR)"
        ),
        "physical-lower-bound": (
            r"STACK_HEAP_ADDR[\s\S]*SOC_PSRAM_DATA_BASE"
            r"|SOC_PSRAM_DATA_BASE[\s\S]*STACK_HEAP_ADDR"
        ),
        "physical-upper-bound": (
            r"STACK_HEAP_ADDR[\s\S]*STACK_HEAP_SIZE[\s\S]*"
            r"SOC_PSRAM_DATA_(?:BASE|SIZE)"
        ),
        "capacity-lower-bound": r"STACK_HEAP_ADDR[\s\S]*CONFIG_PSRAM_BASE",
        "capacity-upper-bound": (
            r"STACK_HEAP_ADDR[\s\S]*STACK_HEAP_SIZE[\s\S]*"
            r"CONFIG_PSRAM_CAPACITY"
        ),
        "ordinary-heap-overlap": r"CONFIG_AP_PSRAM_HEAP_(?:ADDR|SIZE)",
        "section-overlap": r"CONFIG_AP_PSRAM_SECTION_(?:ADDR|SIZE)",
        "text-overlap": r"CONFIG_AP_PSRAM_TEXT_(?:ADDR|SIZE)",
        "slab-overlap": r"CONFIG_PSRAM_MEM_SLAB_",
    }
    return [
        name
        for name, pattern in requirements.items()
        if re.search(pattern, joined, re.MULTILINE) is None
    ]


BASE_CONFIG = {
    "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60700000,
    "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x00010000,
    "CONFIG_CP_PSRAM_HEAP_ADDR": 0x60400000,
    "CONFIG_CP_PSRAM_HEAP_SIZE": 0x00010000,
    "CONFIG_AP_PSRAM_HEAP_ADDR": 0x60600000,
    "CONFIG_AP_PSRAM_HEAP_SIZE": 0x00010000,
    "CONFIG_AP_PSRAM_SECTION_ADDR": 0x60800000,
    "CONFIG_AP_PSRAM_SECTION_SIZE": 0x00010000,
    "CONFIG_AP_PSRAM_TEXT_ADDR": 0x60900000,
    "CONFIG_AP_PSRAM_TEXT_SIZE": 0x00010000,
    "CONFIG_PSRAM_MEM_SLAB_USER_ADDR": 0x60000000,
    "CONFIG_PSRAM_MEM_SLAB_USER_SIZE": 0x00010000,
    "CONFIG_PSRAM_MEM_SLAB_AUDIO_ADDR": 0x60100000,
    "CONFIG_PSRAM_MEM_SLAB_AUDIO_SIZE": 0x00010000,
    "CONFIG_PSRAM_MEM_SLAB_ENCODE_ADDR": 0x60200000,
    "CONFIG_PSRAM_MEM_SLAB_ENCODE_SIZE": 0x00010000,
    "CONFIG_PSRAM_MEM_SLAB_DISPLAY_ADDR": 0x60300000,
    "CONFIG_PSRAM_MEM_SLAB_DISPLAY_SIZE": 0x00010000,
    "SOC_PSRAM_DATA_BASE": 0x60000000,
    "SOC_PSRAM_DATA_SIZE": 0x01000000,
    "CONFIG_PSRAM_BASE": 0x60000000,
    "CONFIG_PSRAM_CAPACITY": 0x01000000,
}


def run_link(
    work: Path,
    empty_object: Path,
    asserts: list[str],
    overrides: dict[str, int],
) -> subprocess.CompletedProcess[str]:
    config = BASE_CONFIG | overrides
    definitions = "\n".join(
        f"#define {name} 0x{value:x}" for name, value in config.items()
    )
    source = (
        f"{definitions}\n"
        "MEMORY { RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x1000 }\n"
        + "\n".join(asserts)
        + "\nSECTIONS { .text : { *(.text*) } > RAM }\n"
    )
    source_path = work / "layout.ld.S"
    script_path = work / "layout.ld"
    source_path.write_text(source, encoding="utf-8")
    preprocess = subprocess.run(
        [str(ARM_CC), "-E", "-P", "-x", "c", str(source_path), "-o", str(script_path)],
        text=True,
        capture_output=True,
        check=False,
    )
    if preprocess.returncode != 0:
        return preprocess
    return subprocess.run(
        [
            str(ARM_CC),
            "-nostdlib",
            f"-Wl,-T,{script_path}",
            str(empty_object),
            "-o",
            str(work / "layout.elf"),
        ],
        text=True,
        capture_output=True,
        check=False,
    )


def main() -> int:
    asserts = extract_stack_asserts(LINKER.read_text(encoding="utf-8"))
    missing = missing_contracts(asserts)
    if missing:
        print(
            "CACHEALLOC-02:LINKER:FAIL missing feature-on ASSERTs: "
            + ",".join(missing)
        )
        return 1

    negative_cases = {
        "zero-size": {"CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0},
        "unaligned-address": {"CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60700001},
        "unaligned-size": {"CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x00010001},
        "addition-overflow": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0xFFFFFFE0,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x40,
        },
        "below-physical": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x5FFFFFE0,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x20,
        },
        "outside-physical": {"CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x61000000},
        # The heap fits the SoC address window but not the memory this project
        # declares as fitted, which is the REF-06 alias-onto-low-PSRAM defect.
        "beyond-declared-capacity": {"CONFIG_PSRAM_CAPACITY": 0x00700000},
        "below-declared-capacity-base": {"CONFIG_PSRAM_BASE": 0x60800000},
        "cross-physical-end": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60FFF000,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x00002000,
        },
        "overlap-ordinary-heap": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60600020,
        },
        "overlap-cp-heap": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60400020,
        },
        "cross-ordinary-left": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x605FFFE0,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x40,
        },
        "cross-ordinary-right": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x6060FFE0,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x40,
        },
        "contain-ordinary-heap": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x605F0000,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x00030000,
        },
        "overlap-section": {"CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60800020},
        "overlap-text": {"CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60900020},
        "overlap-user-slab": {"CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60000020},
        "overlap-audio-slab": {"CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60100020},
        "overlap-encode-slab": {"CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60200020},
        "overlap-display-slab": {"CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60300020},
    }
    positive_cases = {
        "default": {},
        "ordinary-left-adjacent": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x605F0000,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x00010000,
        },
        "ordinary-right-adjacent": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60610000,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x00010000,
        },
        "section-left-adjacent": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x607F0000,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x00010000,
        },
        "physical-upper-edge": {
            "CONFIG_AP_PSRAM_STACK_HEAP_ADDR": 0x60FF0000,
            "CONFIG_AP_PSRAM_STACK_HEAP_SIZE": 0x00010000,
        },
    }

    with tempfile.TemporaryDirectory(prefix="cachealloc-linker-") as directory:
        work = Path(directory)
        empty_source = work / "empty.c"
        empty_object = work / "empty.o"
        empty_source.write_text("void cachealloc_link_probe(void) {}\n", encoding="utf-8")
        compile_result = subprocess.run(
            [str(ARM_CC), "-c", str(empty_source), "-o", str(empty_object)],
            text=True,
            capture_output=True,
            check=False,
        )
        if compile_result.returncode != 0:
            print("CACHEALLOC-02:LINKER:HARNESS " + compile_result.stderr.strip())
            return 2

        for name, overrides in positive_cases.items():
            positive = run_link(work, empty_object, asserts, overrides)
            if positive.returncode != 0:
                print(
                    "CACHEALLOC-02:LINKER:FAIL rejected positive layout: "
                    + name
                )
                print(positive.stderr.strip())
                return 1
        failures: list[str] = []
        for name, overrides in negative_cases.items():
            result = run_link(work, empty_object, asserts, overrides)
            if result.returncode == 0:
                failures.append(name)
        if failures:
            print(
                "CACHEALLOC-02:LINKER:FAIL accepted negative layouts: "
                + ",".join(failures)
            )
            return 1

    print(
        "CACHEALLOC-02:LINKER:PASS "
        f"positive_cases={len(positive_cases)} "
        f"negative_cases={len(negative_cases)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
