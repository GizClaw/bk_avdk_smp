#!/usr/bin/env python3

import re
import subprocess
import sys
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
KERNEL_DIR = TEST_DIR.parents[3]
TASKS_SOURCE = KERNEL_DIR / "tasks.c"
BUILD_DIR = TEST_DIR / "build/fatal-reset"
GENERATED_SOURCE = BUILD_DIR / "fatal_reset_stub_test.c"
TARGET = BUILD_DIR / "fatal_reset_stub_test"


def extract_function(text, name):
    pattern = re.compile(
        rf"\b{re.escape(name)}\s*\([^;{{}}]*\)[^;{{}}]*\{{",
        re.DOTALL,
    )
    match = pattern.search(text)
    if match is None:
        raise ValueError(f"function {name} not found")
    start = text.rfind("\n", 0, match.start()) + 1
    brace = text.find("{", match.start())
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    raise ValueError(f"function {name} has no closing brace")


def generated_test(fatal_function):
    return f"""\
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef int BaseType_t;
typedef int PsramStackFatalReason_t;

typedef struct {{
\tvoid *pxStack;
\tchar pcTaskName[8];
}} TCB_t;

#define STUB_FATAL_STACK 0x60800040u

#define RESET_SOURCE_CRASH_ASSERT 0x0eu
#define portGET_CORE_ID() 1
#define portDISABLE_INTERRUPTS() stub_disable_interrupts()
#define __disable_irq() ((void)stub_disable_interrupts())
#define __DMB() stub_memory_barrier()
#define configPSRAM_STACK_FATAL_RESET() stub_platform_fatal_reset()
#define bk_reboot_ex(reason) stub_forbidden_path()
#define bk_reboot_callback_exe() stub_forbidden_path()
#define ipc_send_cpu1_need_reboot() stub_forbidden_path()

volatile uint32_t ulPsramStackFatalReason;
volatile BaseType_t xPsramStackFatalCore;
volatile uint32_t ulPsramStackFatalStack;
const char * volatile pcPsramStackFatalTask;

static TCB_t s_fatal_tcb = {{ (void *)(uintptr_t)STUB_FATAL_STACK, "wrkr" }};
static TCB_t *pxCurrentTCBs[2] = {{ NULL, &s_fatal_tcb }};

static jmp_buf s_escape;
static bool s_wdt_returns;
static unsigned s_hook_calls;
static unsigned s_dmb_calls;
static unsigned s_set_reason_calls;
static unsigned s_wdt_calls;
static unsigned s_forbidden_calls;
static unsigned s_irq_disable_calls;
static unsigned s_sequence;
static unsigned s_dmb_sequence;
static unsigned s_reason_sequence;
static unsigned s_wdt_sequence;
static unsigned s_irq_sequence;
static bool s_state_recorded_at_dmb;
static bool s_state_recorded_at_hook;
static uint32_t s_reset_reason;

static void stub_memory_barrier(void)
{{
\ts_dmb_calls++;
\ts_dmb_sequence = ++s_sequence;
\ts_state_recorded_at_dmb =
\t\t(ulPsramStackFatalReason == 7u) && (xPsramStackFatalCore == 1)
\t\t&& (ulPsramStackFatalStack == STUB_FATAL_STACK)
\t\t&& (pcPsramStackFatalTask == s_fatal_tcb.pcTaskName);
}}

void stub_platform_fatal_reset(void)
{{
\ts_hook_calls++;
\ts_state_recorded_at_hook =
\t\t(ulPsramStackFatalReason == 7u)
\t\t&& (xPsramStackFatalCore == 1)
\t\t&& (ulPsramStackFatalStack == STUB_FATAL_STACK)
\t\t&& (pcPsramStackFatalTask == s_fatal_tcb.pcTaskName)
\t\t&& (s_dmb_calls == 1u);
\ts_set_reason_calls++;
\ts_reset_reason = RESET_SOURCE_CRASH_ASSERT;
\ts_reason_sequence = ++s_sequence;
\ts_wdt_calls++;
\ts_wdt_sequence = ++s_sequence;
\tif (!s_wdt_returns) {{
\t\tlongjmp(s_escape, 1);
\t}}
}}

void stub_forbidden_path(void)
{{
\ts_forbidden_calls++;
}}

static unsigned stub_disable_interrupts(void)
{{
\ts_irq_disable_calls++;
\ts_irq_sequence = ++s_sequence;
\tlongjmp(s_escape, 2);
\treturn 0u;
}}

{fatal_function}

static void reset_state(bool wdt_returns)
{{
\ts_wdt_returns = wdt_returns;
\ts_hook_calls = 0u;
\ts_dmb_calls = 0u;
\ts_set_reason_calls = 0u;
\ts_wdt_calls = 0u;
\ts_forbidden_calls = 0u;
\ts_irq_disable_calls = 0u;
\ts_sequence = 0u;
\ts_dmb_sequence = 0u;
\ts_reason_sequence = 0u;
\ts_wdt_sequence = 0u;
\ts_irq_sequence = 0u;
\ts_state_recorded_at_dmb = false;
\ts_state_recorded_at_hook = false;
\ts_reset_reason = 0u;
\tulPsramStackFatalReason = 0u;
\txPsramStackFatalCore = -1;
\tulPsramStackFatalStack = 0u;
\tpcPsramStackFatalTask = NULL;
}}

static unsigned run_scenario(bool wdt_returns)
{{
\tunsigned failures = 0u;
\tint escape;

\treset_state(wdt_returns);
\tescape = setjmp(s_escape);
\tif (escape == 0) {{
\t\tprvPsramStackKernelFatal((PsramStackFatalReason_t)7);
\t}}

\tif (s_hook_calls != 1u
\t    || s_dmb_calls != 1u
\t    || s_set_reason_calls != 1u
\t    || s_wdt_calls != 1u
\t    || s_forbidden_calls != 0u
\t    || s_reset_reason != RESET_SOURCE_CRASH_ASSERT
\t    || !s_state_recorded_at_dmb
\t    || !s_state_recorded_at_hook
\t    || s_dmb_sequence >= s_reason_sequence
\t    || s_reason_sequence >= s_wdt_sequence) {{
\t\tfailures++;
\t}}
\tif (!wdt_returns) {{
\t\tif (escape != 1 || s_irq_disable_calls != 0u) {{
\t\t\tfailures++;
\t\t}}
\t}} else if (escape != 2
\t\t   || s_irq_disable_calls != 1u
\t\t   || s_wdt_sequence >= s_irq_sequence) {{
\t\tfailures++;
\t}}
\treturn failures;
}}

int main(void)
{{
\tunsigned failures = run_scenario(false) + run_scenario(true);

\tif (failures != 0u) {{
\t\tprintf("RTOS-04:FATAL-STUB:FAIL failures=%u hook=%u dmb=%u "
\t\t       "state_at_dmb=%u state_at_hook=%u set_reason=%u wdt=%u "
\t\t       "forbidden=%u irq_fallback=%u\\n",
\t\t       failures, s_hook_calls, s_dmb_calls,
\t\t       s_state_recorded_at_dmb ? 1u : 0u,
\t\t       s_state_recorded_at_hook ? 1u : 0u, s_set_reason_calls,
\t\t       s_wdt_calls, s_forbidden_calls, s_irq_disable_calls);
\t\treturn 1;
\t}}
\tprintf("RTOS-04:FATAL-STUB:PASS scenarios=2 hook=1 dmb=1 "
\t       "state_at_dmb=1 state_at_hook=1 set_reason=1 reason=0xe "
\t       "wdt=1 forbidden=0 irq_fallback=1\\n");
\treturn 0;
}}
"""


def main():
    try:
        tasks = TASKS_SOURCE.read_text(encoding="utf-8")
        fatal_function = extract_function(tasks, "prvPsramStackKernelFatal")
        BUILD_DIR.mkdir(parents=True, exist_ok=True)
        GENERATED_SOURCE.write_text(
            generated_test(fatal_function),
            encoding="utf-8",
        )
        compile_result = subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wshadow",
                "-Wno-pointer-to-int-cast",
                str(GENERATED_SOURCE),
                "-o",
                str(TARGET),
            ],
            text=True,
            capture_output=True,
        )
        if compile_result.returncode != 0:
            sys.stderr.write(compile_result.stdout)
            sys.stderr.write(compile_result.stderr)
            print("RTOS-04:FATAL-STUB:HARNESS-FAIL compile", file=sys.stderr)
            return 2
        try:
            return subprocess.run(
                [str(TARGET)],
                check=False,
                timeout=5,
            ).returncode
        except subprocess.TimeoutExpired:
            print(
                "RTOS-04:FATAL-STUB:FAIL WDT returned without IRQ fallback",
                file=sys.stderr,
            )
            return 1
    except (OSError, ValueError) as error:
        print(f"RTOS-04:FATAL-STUB:HARNESS-FAIL {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
