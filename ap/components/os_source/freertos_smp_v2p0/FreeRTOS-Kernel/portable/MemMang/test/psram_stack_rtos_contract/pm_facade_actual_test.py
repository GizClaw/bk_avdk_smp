#!/usr/bin/env python3

import json
import re
import shlex
import subprocess
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
SDK_DIR = next(parent.parent for parent in TEST_DIR.parents if parent.name == "ap")
SOURCE = SDK_DIR / "ap/components/bk_rtos/freertos/v10/psram_stack_pm.c"
HEADER = SDK_DIR / "ap/components/bk_rtos/freertos/v10/psram_stack_pm.h"
COMPILE_DB = SDK_DIR / "build/bk7258/platform/bk7258_ap/compile_commands.json"


def extract_function(source, name):
    match = re.search(rf"\b{name}\s*\([^;]*\)\s*\{{", source)
    if match is None:
        return ""
    start = match.start()
    brace = source.find("{", match.start())
    depth = 0
    for offset in range(brace, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start : offset + 1]
    return ""


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
    missing = []
    if not SOURCE.is_file():
        missing.append("production-psram-stack-pm-source")
    if not HEADER.is_file():
        missing.append("production-psram-stack-pm-private-header")
    if missing:
        for item in missing:
            print(f"PM-05B:FACADE:MISSING {item}", file=sys.stderr)
        print(f"PM-05B:FACADE:FAIL checks=2 missing={len(missing)}")
        return 1

    try:
        source = SOURCE.read_text(encoding="utf-8")
        entries = json.loads(COMPILE_DB.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"PM-05B:FACADE:HARNESS-FAIL {error}", file=sys.stderr)
        return 2

    matches = [
        entry
        for entry in entries
        if Path(entry["file"]).resolve() == SOURCE.resolve()
    ]
    if not matches:
        matches = [
            entry
            for entry in entries
            if Path(entry["file"]).name == "rtos_pub_smp.c"
            and "/ap/components/bk_rtos/" in Path(entry["file"]).as_posix()
        ]
    if len(matches) != 1:
        print(
            "PM-05B:FACADE:MISSING actual-facade-compile-command",
            file=sys.stderr,
        )
        return 1
    command = sanitized_command(matches[0])
    command = [
        str(SOURCE)
        if Path(argument).name == Path(matches[0]["file"]).name
        else argument
        for argument in command
    ]
    result = subprocess.run(
        command
        + [
            "-UCONFIG_CACHEABLE_PSRAM_TASK_STACK",
            "-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1",
            "-Werror",
            "-fsyntax-only",
        ],
        cwd=matches[0]["directory"],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        print("PM-05B:FACADE:FAIL actual-source-arm-compile")
        return 1

    vote = extract_function(source, "bk_rtos_psram_stack_pm_vote")
    checks = {
        "no-task-owned-mutex-or-participant": not any(
            token in source
            for token in (
                "StaticSemaphore_t",
                "xSemaphoreCreateMutexStatic",
                "xSemaphoreTake",
                "xSemaphoreGive",
                "xTaskGetCurrentTaskHandle",
                "lifecycle_owner",
                "participant",
            )
        ),
        "resync-claim-commit-abort-use-uint64": all(
            re.search(pattern, source) is not None
            for pattern in (
                r"resync_claim\s*\([^)]*uint64_t",
                r"resync_commit\s*\([^)]*uint64_t",
                r"resync_abort\s*\([^)]*uint64_t",
            )
        ),
        "uint64-generation": "uint64_t" in source,
        "dedicated-psram-stack-transaction": (
            "bk_pm_psram_stack_vote_sleep_ctrl" in vote
        ),
        "lockout-resync-and-inflight": all(
            token in source for token in ("lockout", "resync", "inflight")
        ),
        "pm-call-has-no-heap-or-kernel-lock": bool(vote)
        and all(
            token not in vote
            for token in ("HeapEnterCritical", "xKernelLock", "taskENTER_CRITICAL")
        ),
        "nack-timeout-result-propagation": "result = " in source
        and "return result;" in source,
        "pending-hold-blocks-active-until-shared-resync": (
            re.search(r"pending[\w_]*generation", source) is not None
            and re.search(
                r"pending[\w_]*generation[\s\S]*?(?:return\s+BK_FAIL|lockout)",
                source,
            )
            is not None
        ),
    }
    for name, passed in checks.items():
        if not passed:
            print(f"PM-05B:FACADE:MISSING {name}", file=sys.stderr)
    failures = sum(not passed for passed in checks.values())
    if failures:
        print(
            f"PM-05B:FACADE:FAIL checks={len(checks)} missing={failures}"
        )
        return 1
    print(
        f"PM-05B:FACADE:PASS checks={len(checks)} missing=0 arm=1"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
