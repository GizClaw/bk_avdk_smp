#!/usr/bin/env python3

import json
import shlex
import subprocess
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
SDK_DIR = next(parent.parent for parent in TEST_DIR.parents if parent.name == "ap")
COMPILE_DB = SDK_DIR / "build/bk7258/platform/bk7258_ap/compile_commands.json"
TASKS_SUFFIX = (
    "ap/components/os_source/freertos_smp_v2p0/FreeRTOS-Kernel/tasks.c"
)


def sanitized_command(entry):
    arguments = entry.get("arguments")
    args = list(arguments) if arguments is not None else shlex.split(entry["command"])
    output = []
    skip_next = False
    for argument in args:
        if skip_next:
            skip_next = False
            continue
        if argument in {"-o", "-MF", "-MT", "-MQ"}:
            skip_next = True
            continue
        if argument in {"-c", "-MD", "-MMD", "-MP"}:
            continue
        if argument.startswith(("-o", "-MF", "-MT", "-MQ")):
            continue
        output.append(argument)
    return output


def main():
    try:
        entries = json.loads(COMPILE_DB.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"PM-05B:TLS:HARNESS-FAIL {error}", file=sys.stderr)
        return 2
    matches = [
        entry
        for entry in entries
        if Path(entry["file"]).as_posix().endswith(TASKS_SUFFIX)
    ]
    if len(matches) != 1:
        print(
            f"PM-05B:TLS:HARNESS-FAIL tasks compile count={len(matches)}",
            file=sys.stderr,
        )
        return 2
    entry = matches[0]
    result = subprocess.run(
        sanitized_command(entry)
        + [
            "-UCONFIG_CACHEABLE_PSRAM_TASK_STACK",
            "-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1",
            "-UconfigUSE_C_RUNTIME_TLS_SUPPORT",
            "-DconfigUSE_C_RUNTIME_TLS_SUPPORT=1",
            "-Werror",
            "-fsyntax-only",
        ],
        cwd=entry["directory"],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        print("PM-05B:TLS:FAIL feature_on_tls_on_arm_compile=0")
        return 1
    print("PM-05B:TLS:PASS feature_on_tls_on_arm_compile=1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
