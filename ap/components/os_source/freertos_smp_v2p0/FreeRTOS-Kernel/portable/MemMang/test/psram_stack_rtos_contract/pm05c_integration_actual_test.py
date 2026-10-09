#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
SDK = HERE.parents[8]
FACADE = SDK / "ap/components/bk_rtos/freertos/v10/psram_stack_pm.c"
AP_PM = SDK / "ap/middleware/driver/pwr_clk/pwr_clk.c"
CP_CORE = SDK / "cp/middleware/driver/pwr_clk/low_pwr_core.c"
BUILD = HERE / "build"


def extract_function(text, name):
    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{}}]*\)[^;{{}}]*\{{", text)
    if match is None:
        return ""
    start = text.rfind("\n", 0, match.start()) + 1
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


def extract_case_body(text, marker):
    match = re.search(
        rf"case\s+{re.escape(marker)}\s*:\s*\{{", text
    )
    if match is None:
        return ""
    brace = text.find("{", match.start())
    depth = 0
    for position in range(brace, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1 : position]
    return ""


def harness(facade_vote, ap_functions, cp_body):
    return f"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int bk_err_t;
#define BK_OK 0
#define BK_FAIL (-1)
#define PM_SLEEP_MODULE_NAME_PSRAM 7U
#define PM_PSRAM_STACK_SLEEP_CTRL_CMD 0x11U
#define PM_SEND_CMD_CP1_RESPONSE_TIEM 2U
#define PM_AON_RTC_DEFAULT_TICK_COUNT 1U
typedef enum {{
    PM_PSRAM_STACK_IDLE = 0, PM_PSRAM_STACK_PREPARING,
    PM_PSRAM_STACK_WAITING, PM_PSRAM_STACK_TIMED_OUT,
    PM_PSRAM_STACK_DONE
}} pm_psram_stack_proxy_state_e;
typedef struct {{ uint32_t param1; uint32_t param2; uint32_t param3; }} msg_t;
static uint32_t s_pm_psram_stack_state;
static uint32_t s_pm_psram_stack_request_id;
static uint32_t s_pm_psram_stack_request_generation;
static bk_err_t s_pm_psram_stack_result = BK_FAIL;
static bool s_pm_psram_stack_locked_out;
static uint32_t sent_cmd;
static uint32_t cp_vote_calls;
static uint32_t ack_calls;
static uint64_t tick;
static void pm_psram_stack_ack(uint32_t request_id, bk_err_t result);
bk_err_t bk_pm_psram_stack_vote_sleep_ctrl(uint32_t sleep_state);
static bk_err_t bk_pm_module_vote_sleep_ctrl(
    uint32_t module, uint32_t state, uint32_t arg)
{{
    (void)module; (void)state; (void)arg; cp_vote_calls++; return BK_OK;
}}
static void bk_pm_cp0_response_cp1(
    uint32_t cmd, uint32_t p1, uint32_t p2, uint32_t p3)
{{
    (void)p3;
    if (cmd == PM_PSRAM_STACK_SLEEP_CTRL_CMD) {{
        ack_calls++;
        pm_psram_stack_ack(p1, (bk_err_t)p2);
    }}
}}
static void cp_handle(msg_t msg)
{{
    bk_err_t ret = BK_FAIL;
    {cp_body}
    (void)ret;
}}
static int pm_cp1_mailbox_send_data(
    uint32_t cmd, uint32_t p1, uint32_t p2, uint32_t p3)
{{
    sent_cmd = cmd;
    cp_handle((msg_t){{p1, p2, p3}});
    return BK_OK;
}}
static uint64_t pm_cp1_aon_rtc_counter_get(void) {{ return tick++; }}
static bool rtos_is_in_interrupt_context(void) {{ return false; }}
static bool rtos_local_irq_disabled(void) {{ return false; }}
{ap_functions}
{facade_vote}
int main(void)
{{
    const bk_err_t result = bk_rtos_psram_stack_pm_vote(0U);
    const unsigned failures =
        (result != BK_OK)
        || (sent_cmd != PM_PSRAM_STACK_SLEEP_CTRL_CMD)
        || (cp_vote_calls != 1U)
        || (ack_calls != 1U);
    printf(
        "result=%d cmd=%u cp_vote=%u ack=%u failures=%u\\n",
        result, sent_cmd, cp_vote_calls, ack_calls, failures);
    return failures == 0U ? 0 : 1;
}}
"""


def main():
    try:
        facade = FACADE.read_text(encoding="utf-8")
        ap = AP_PM.read_text(encoding="utf-8")
        cp = CP_CORE.read_text(encoding="utf-8")
    except OSError as error:
        print(f"PM-05C:INTEGRATION:HARNESS-FAIL {error}", file=sys.stderr)
        return 2
    facade_vote = extract_function(facade, "bk_rtos_psram_stack_pm_vote")
    ap_functions = "\n".join(
        extract_function(ap, name)
        for name in (
            "pm_psram_stack_begin",
            "pm_psram_stack_quarantine",
            "pm_psram_stack_ack",
            "pm_psram_stack_complete",
            "bk_pm_psram_stack_vote_sleep_ctrl",
        )
    )
    cp_body = extract_case_body(cp, "LOW_PWR_CORE_PSRAM_STACK_SLEEP")
    if not facade_vote or not ap_functions or not cp_body:
        print("PM-05C:INTEGRATION:MISSING production-path", file=sys.stderr)
        return 1
    BUILD.mkdir(parents=True, exist_ok=True)
    source = BUILD / "pm05c_integration_actual.c"
    binary = BUILD / "pm05c_integration_actual"
    source.write_text(harness(facade_vote, ap_functions, cp_body), encoding="utf-8")
    compile_result = subprocess.run(
        [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(source), "-o", str(binary),
        ],
        text=True,
        capture_output=True,
    )
    if compile_result.returncode != 0:
        sys.stderr.write(compile_result.stdout + compile_result.stderr)
        print("PM-05C:INTEGRATION:FAIL compile=1")
        return 1
    run_result = subprocess.run([str(binary)], text=True, capture_output=True)
    sys.stdout.write(run_result.stdout + run_result.stderr)
    print(
        f"PM-05C:INTEGRATION:{'PASS' if run_result.returncode == 0 else 'FAIL'} "
        "production_segments=3"
    )
    return run_result.returncode


if __name__ == "__main__":
    sys.exit(main())
