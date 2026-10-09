#!/usr/bin/env python3

import sys
import subprocess
from pathlib import Path

from arm_feature_on_source_test import (
    feature_flags_for,
    load_entries,
    sanitized_command,
)


TEST_DIR = Path(__file__).resolve().parent
BUILD_DIR = TEST_DIR / "build/arm-source"
FACADE = (
    TEST_DIR.parents[6]
    / "bk_rtos/freertos/v10/psram_stack_pm.c"
)


def compile_facade_variants():
    entry = load_entries()["wrapper"]
    source = str(Path(entry["file"]).resolve())
    outputs = {}
    base = [
        str(FACADE) if str(Path(argument).resolve()) == source else argument
        for argument in sanitized_command(entry)
    ]
    for enabled in (False, True):
        result = subprocess.run(
            base
            + feature_flags_for(enabled)
            + ["-Werror", "-fsyntax-only"],
            cwd=entry["directory"],
            text=True,
            capture_output=True,
        )
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            raise RuntimeError(
                f"facade feature-{int(enabled)} ARM compile failed"
            )
        preprocess = subprocess.run(
            base
            + feature_flags_for(enabled)
            + ["-E", "-P"],
            cwd=entry["directory"],
            text=True,
            capture_output=True,
        )
        if preprocess.returncode != 0:
            sys.stderr.write(preprocess.stdout)
            sys.stderr.write(preprocess.stderr)
            raise RuntimeError(
                f"facade feature-{int(enabled)} preprocess failed"
            )
        outputs[enabled] = preprocess.stdout
    return outputs


def main():
    try:
        facade_outputs = compile_facade_variants()
        tasks_on = (BUILD_DIR / "tasks.feature_on.i").read_text(encoding="utf-8")
        tasks_off = (BUILD_DIR / "tasks.feature_off.i").read_text(encoding="utf-8")
        wrapper_on = (BUILD_DIR / "wrapper.feature_on.i").read_text(encoding="utf-8")
        wrapper_off = (BUILD_DIR / "wrapper.feature_off.i").read_text(
            encoding="utf-8"
        )
    except (OSError, RuntimeError, ValueError) as error:
        print(f"PM-05B:ARM:HARNESS-FAIL {error}", file=sys.stderr)
        return 2

    missing = []
    for token in (
        "psram_stack_alloc_with_transition",
        "psram_stack_free_exact_with_transition",
        "psram_stack_pm_hold_release_commit",
        "bk_rtos_psram_stack_pm",
    ):
        if token not in tasks_on:
            missing.append(f"tasks-feature-on-{token}")
    for token in ("bk_rtos_psram_stack_pm",):
        if token not in wrapper_on:
            missing.append(f"wrapper-feature-on-{token}")
    for token in (
        "PM_SLEEP_MODULE_NAME_PSRAM",
        "bk_pm_module_vote_sleep_ctrl",
        "bk_rtos_psram_stack_pm",
    ):
        if token not in facade_outputs[True]:
            missing.append(f"facade-feature-on-{token}")
    if "bk_rtos_psram_stack_pm" in tasks_off or "bk_rtos_psram_stack_pm" in wrapper_off:
        missing.append("feature-off-pm-path-not-zero")
    if "bk_rtos_psram_stack_pm" in facade_outputs[False]:
        missing.append("facade-feature-off-pm-path-not-zero")

    for item in missing:
        print(f"PM-05B:ARM:MISSING {item}", file=sys.stderr)
    if missing:
        print(
            f"PM-05B:ARM:FAIL compiled=5 preprocessed=8 missing={len(missing)}"
        )
        return 1
    print(
        "PM-05B:ARM:PASS compiled=5 preprocessed=8 "
        "facade_feature_on_off=2 missing=0"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
