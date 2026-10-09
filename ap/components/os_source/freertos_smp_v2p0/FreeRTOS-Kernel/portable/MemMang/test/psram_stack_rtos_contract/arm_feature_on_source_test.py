#!/usr/bin/env python3

import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

FEATURE = "CONFIG_CACHEABLE_PSRAM_TASK_STACK"
TEST_DIR = Path(__file__).resolve().parent
SDK_DIR = next(parent.parent for parent in TEST_DIR.parents if parent.name == "ap")
COMPILE_DB = SDK_DIR / "build/bk7258/platform/bk7258_ap/compile_commands.json"
BUILD_DIR = TEST_DIR / "build/arm-source"
TARGETS = {
    "tasks": (
        "ap/components/os_source/freertos_smp_v2p0/"
        "FreeRTOS-Kernel/tasks.c"
    ),
    "port": (
        "ap/components/os_source/freertos_smp_v2p0/FreeRTOS-Kernel/"
        "portable/GCC/ARM_CM33_NTZ/non_secure/port.c"
    ),
    "wrapper": "ap/components/bk_rtos/freertos/v10/rtos_pub_smp.c",
}


def load_entries():
    entries = json.loads(COMPILE_DB.read_text(encoding="utf-8"))
    selected = {}
    for name, suffix in TARGETS.items():
        matches = [
            entry
            for entry in entries
            if Path(entry["file"]).as_posix().endswith(suffix)
        ]
        if len(matches) != 1:
            raise ValueError(f"{name} compile command count is {len(matches)}")
        selected[name] = matches[0]
    return selected


def sanitized_command(entry):
    arguments = entry.get("arguments")
    args = list(arguments) if arguments is not None else shlex.split(entry["command"])
    output = []
    skip_next = False
    paired = {"-o", "-MF", "-MT", "-MQ"}
    standalone = {"-c", "-MD", "-MMD", "-MP"}

    for argument in args:
        if skip_next:
            skip_next = False
            continue
        if argument in paired:
            skip_next = True
            continue
        if argument in standalone:
            continue
        if argument.startswith(("-o", "-MF", "-MT", "-MQ")):
            continue
        output.append(argument)
    return output


def feature_flags_for(enabled):
    """Force the feature macro to `enabled` for one variant.

    The compile database force-includes a generated `sdkconfig.h` whose value
    depends on whichever configuration was built last, so a plain `-D` would
    collide with it under `-Werror`. A trailing `-include` is processed after
    `sdkconfig.h` and therefore wins for either build configuration.
    """
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    override = BUILD_DIR / f"feature_override_{int(enabled)}.h"
    override.write_text(
        f"#undef {FEATURE}\n#define {FEATURE} {int(enabled)}\n",
        encoding="utf-8",
    )
    return [f"-include{override}"]


def run_variant(name, entry, enabled):
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    base = sanitized_command(entry)
    feature_flags = feature_flags_for(enabled)
    mode = "on" if enabled else "off"
    preprocessed = BUILD_DIR / f"{name}.feature_{mode}.i"

    if enabled:
        compile_result = subprocess.run(
            base + feature_flags + ["-Werror", "-fsyntax-only"],
            cwd=entry["directory"],
            text=True,
            capture_output=True,
        )
        if compile_result.returncode != 0:
            sys.stderr.write(compile_result.stdout)
            sys.stderr.write(compile_result.stderr)
            raise RuntimeError(f"{name} feature-on ARM compile failed")

    preprocess_result = subprocess.run(
        base + feature_flags + ["-E", "-P", "-o", str(preprocessed)],
        cwd=entry["directory"],
        text=True,
        capture_output=True,
    )
    if preprocess_result.returncode != 0:
        sys.stderr.write(preprocess_result.stdout)
        sys.stderr.write(preprocess_result.stderr)
        raise RuntimeError(f"{name} feature-{mode} preprocess failed")
    return preprocessed.read_text(encoding="utf-8")


def extract_typedef(text, tag, typedef_name):
    start_match = re.search(rf"typedef\s+struct\s+{re.escape(tag)}\b", text)
    if start_match is None:
        raise ValueError(f"{tag} not found in preprocessed tasks.c")
    end_match = re.search(
        rf"\}}\s*{re.escape(typedef_name)}\s*;",
        text[start_match.start() :],
    )
    if end_match is None:
        raise ValueError(f"{typedef_name} end not found in preprocessed tasks.c")
    end = start_match.start() + end_match.end()
    return re.sub(r"\s+", " ", text[start_match.start() : end]).strip()


def main():
    try:
        entries = load_entries()
        outputs = {
            name: {
                "off": run_variant(name, entry, False),
                "on": run_variant(name, entry, True),
            }
            for name, entry in entries.items()
        }
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"RTOS-04:ARM-SOURCE:HARNESS-FAIL {error}", file=sys.stderr)
        return 2

    missing = []
    markers = {
        "tasks": ("vTaskSwitchContext", "xTaskCreateInPsram"),
        "port": ("vPortSVCHandler_C", "vTaskCacheAcquireFirstStack"),
        "wrapper": ("rtos_create_thread", "xTaskCreateInPsram"),
    }
    for name, tokens in markers.items():
        for token in tokens:
            if token not in outputs[name]["on"]:
                missing.append(f"{name}-feature-on-marker-{token}")

    if "prvAllocateCacheablePsramTask" not in outputs["tasks"]["on"]:
        missing.append("additions-not-preprocessed-through-tasks-c")
    if "prvAllocateCacheablePsramTask" in outputs["tasks"]["off"]:
        missing.append("additions-feature-off-path-not-zero")

    for tag, typedef_name, case_name in (
        ("tskTaskControlBlock", "tskTCB", "tcb-feature-on-off-layout"),
        ("xSTATIC_TCB", "StaticTask_t", "static-task-feature-on-off-layout"),
    ):
        off_layout = extract_typedef(outputs["tasks"]["off"], tag, typedef_name)
        on_layout = extract_typedef(outputs["tasks"]["on"], tag, typedef_name)
        if off_layout != on_layout:
            missing.append(case_name)

    for item in missing:
        print(f"RTOS-04:ARM-SOURCE:MISSING {item}")
    if missing:
        print(
            "RTOS-04:ARM-SOURCE:FAIL "
            f"compiled=3 preprocessed=6 missing={len(missing)}"
        )
        return 1

    print("RTOS-04:ARM-SOURCE:PASS compiled=3 preprocessed=6 missing=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
