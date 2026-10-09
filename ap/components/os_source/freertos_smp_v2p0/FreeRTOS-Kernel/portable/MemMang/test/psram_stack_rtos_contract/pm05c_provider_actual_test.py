#!/usr/bin/env python3

import re
import subprocess
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
SDK_DIR = next(parent.parent for parent in TEST_DIR.parents if parent.name == "ap")
AP_SOURCE = SDK_DIR / "ap/middleware/driver/pwr_clk/pwr_clk.c"
CP_SOURCE = SDK_DIR / "cp/middleware/driver/pwr_clk/low_pwr_core.c"
BUILD_DIR = TEST_DIR / "build"


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
    start = text.find(f"case {marker}:")
    if start < 0:
        return ""
    brace = text.find("{", start)
    if brace < 0:
        return ""
    depth = 0
    for position in range(brace, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1 : position]
    return ""


def compile_and_run(name, source, defines=()):
    source_path = BUILD_DIR / f"{name}.c"
    binary_path = BUILD_DIR / name
    source_path.write_text(source, encoding="utf-8")
    compile_result = subprocess.run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            *[f"-D{define}" for define in defines],
            str(source_path),
            "-o",
            str(binary_path),
        ],
        text=True,
        capture_output=True,
    )
    if compile_result.returncode != 0:
        sys.stderr.write(compile_result.stdout)
        sys.stderr.write(compile_result.stderr)
        return 2, ""
    run_result = subprocess.run(
        [str(binary_path)],
        text=True,
        capture_output=True,
    )
    sys.stderr.write(run_result.stderr)
    return run_result.returncode, run_result.stdout


def ap_harness(functions):
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
    PM_PSRAM_STACK_IDLE = 0,
    PM_PSRAM_STACK_PREPARING,
    PM_PSRAM_STACK_WAITING,
    PM_PSRAM_STACK_TIMED_OUT,
    PM_PSRAM_STACK_DONE,
}} pm_psram_stack_proxy_state_e;
static uint32_t s_pm_psram_stack_state;
static uint32_t s_pm_psram_stack_request_id;
static uint32_t s_pm_psram_stack_request_generation;
static bk_err_t s_pm_psram_stack_result = BK_FAIL;
static bool s_pm_psram_stack_locked_out;
static uint32_t sent_cmd;
static uint32_t sent_request;
static uint32_t send_calls;
static bk_err_t send_result = BK_OK;
static uint64_t tick;
static bool deliver_ack;
static bk_err_t delivered_result;
static void pm_psram_stack_ack(uint32_t request_id, bk_err_t result);
static int pm_cp1_mailbox_send_data(
    uint32_t cmd, uint32_t p1, uint32_t p2, uint32_t p3)
{{
    (void)p1; (void)p2;
    sent_cmd = cmd; sent_request = p3; send_calls++; return send_result;
}}
static uint64_t pm_cp1_aon_rtc_counter_get(void)
{{
    tick++;
    if (deliver_ack && tick == 2U) {{
        pm_psram_stack_ack(sent_request, delivered_result);
    }}
    return tick;
}}
{functions}
static void reset_transaction(void)
{{
    s_pm_psram_stack_state = PM_PSRAM_STACK_IDLE;
    s_pm_psram_stack_request_id = 0U;
    s_pm_psram_stack_result = BK_FAIL;
    tick = 0U;
    deliver_ack = false;
    send_result = BK_OK;
}}
int main(void)
{{
    unsigned failures = 0U;
    reset_transaction();
    deliver_ack = true;
    delivered_result = BK_OK;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_OK)
        || sent_cmd != PM_PSRAM_STACK_SLEEP_CTRL_CMD) {{
        failures++;
    }}
    reset_transaction();
    deliver_ack = true;
    delivered_result = -7;
    if (bk_pm_psram_stack_vote_sleep_ctrl(1U) == BK_OK) {{
        failures++;
    }}
    reset_transaction();
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_FAIL)
        || s_pm_psram_stack_state != PM_PSRAM_STACK_TIMED_OUT) {{
        failures++;
    }}
    const uint32_t timed_out_request = s_pm_psram_stack_request_id;
    pm_psram_stack_ack(timed_out_request + 1U, BK_OK);
    if (s_pm_psram_stack_state != PM_PSRAM_STACK_TIMED_OUT) {{
        failures++;
    }}
    pm_psram_stack_ack(timed_out_request, BK_OK);
    if (s_pm_psram_stack_state != PM_PSRAM_STACK_IDLE) {{
        failures++;
    }}
    s_pm_psram_stack_state = PM_PSRAM_STACK_WAITING;
    if (bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_FAIL) {{
        failures++;
    }}
    reset_transaction();
    send_result = BK_FAIL;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_FAIL)
        || s_pm_psram_stack_state != PM_PSRAM_STACK_IDLE) {{
        failures++;
    }}
    send_result = BK_OK;
    deliver_ack = true;
    delivered_result = BK_OK;
    tick = 0U;
    if (bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_OK) {{
        failures++;
    }}
    s_pm_psram_stack_state = PM_PSRAM_STACK_IDLE;
    s_pm_psram_stack_request_generation = UINT32_MAX;
    const uint32_t sends_before_exhaustion = send_calls;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_FAIL)
        || !s_pm_psram_stack_locked_out
        || send_calls != sends_before_exhaustion) {{
        failures++;
    }}
    printf("ap_failures=%u\\n", failures);
    return failures == 0U ? 0 : 1;
}}
"""


def ap_feature_on_harness(functions):
    return f"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef int bk_err_t;
#define BK_OK 0
#define BK_FAIL (-1)
#define PM_SLEEP_MODULE_NAME_PSRAM 7U
#define PM_PSRAM_STACK_SLEEP_CTRL_CMD 0x11U
#define PM_SEND_CMD_CP1_RESPONSE_TIEM 8U
#define PM_AON_RTC_DEFAULT_TICK_COUNT 1U
typedef enum {{
    PM_PSRAM_STACK_IDLE = 0,
    PM_PSRAM_STACK_PREPARING,
    PM_PSRAM_STACK_WAITING,
    PM_PSRAM_STACK_TIMED_OUT,
    PM_PSRAM_STACK_DONE,
}} pm_psram_stack_proxy_state_e;
typedef enum {{
    PM_PSRAM_STACK_UNSYNCED = 0,
    PM_PSRAM_STACK_RELEASED,
    PM_PSRAM_STACK_ACTIVE,
    PM_PSRAM_STACK_LOCKED_OUT,
}} pm_psram_stack_stable_state_e;
static uint32_t s_pm_psram_stack_state;
static uint32_t s_pm_psram_stack_request_id;
static uint32_t s_pm_psram_stack_request_generation;
static uint32_t s_pm_psram_stack_request_sleep_state;
static uint32_t s_pm_psram_stack_source_stable;
static uint32_t s_pm_psram_stack_stable_state;
static bk_err_t s_pm_psram_stack_result = BK_FAIL;
static bool s_pm_psram_stack_locked_out;
static uint32_t sent_request;
static uint32_t send_calls;
static bk_err_t send_result = BK_OK;
static uint64_t tick;
static bool deliver_ack;
static uint64_t ack_tick;
static bk_err_t delivered_result;
static bool peer_ack_pending;
static uint32_t peer_ack_request;
static uint64_t peer_ack_tick;
static void pm_psram_stack_ack(uint32_t request_id, bk_err_t result);
static int pm_cp1_mailbox_send_data(
    uint32_t cmd, uint32_t p1, uint32_t p2, uint32_t p3)
{{
    (void)cmd; (void)p1; (void)p2;
    sent_request = p3; send_calls++; return send_result;
}}
static uint64_t pm_cp1_aon_rtc_counter_get(void)
{{
    tick++;
    if (peer_ack_pending && (tick >= peer_ack_tick)) {{
        peer_ack_pending = false;
        pm_psram_stack_ack(peer_ack_request, BK_OK);
    }}
    if (deliver_ack && (send_calls > 0U) && (tick >= ack_tick)) {{
        deliver_ack = false;
        pm_psram_stack_ack(sent_request, delivered_result);
    }}
    return tick;
}}
{functions}
static void reset_transaction(void)
{{
    s_pm_psram_stack_state = PM_PSRAM_STACK_IDLE;
    s_pm_psram_stack_request_id = 0U;
    s_pm_psram_stack_request_generation = 0U;
    s_pm_psram_stack_request_sleep_state = 0U;
    s_pm_psram_stack_source_stable = PM_PSRAM_STACK_UNSYNCED;
    s_pm_psram_stack_stable_state = PM_PSRAM_STACK_UNSYNCED;
    s_pm_psram_stack_result = BK_FAIL;
    s_pm_psram_stack_locked_out = false;
    sent_request = 0U;
    send_calls = 0U;
    send_result = BK_OK;
    tick = 0U;
    deliver_ack = false;
    ack_tick = 1U;
    delivered_result = BK_OK;
    peer_ack_pending = false;
    peer_ack_request = 0U;
    peer_ack_tick = 3U;
}}
int main(void)
{{
    unsigned failures = 0U;

    /* An acknowledged vote records the stable state it converged to. */
    reset_transaction();
    deliver_ack = true;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_OK)
        || (s_pm_psram_stack_stable_state != PM_PSRAM_STACK_ACTIVE)
        || (send_calls != 1U)) {{
        failures++;
    }}

    /* Re-voting the stable state costs no transaction. */
    reset_transaction();
    s_pm_psram_stack_stable_state = PM_PSRAM_STACK_ACTIVE;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_OK)
        || (send_calls != 0U)) {{
        failures++;
    }}

    /* A peer transaction already converging on the requested state must be
     * adopted rather than reported as a failure, otherwise a concurrent
     * cacheable task creation is rejected as out of memory. */
    reset_transaction();
    s_pm_psram_stack_state = PM_PSRAM_STACK_WAITING;
    s_pm_psram_stack_request_id = 41U;
    s_pm_psram_stack_request_generation = 41U;
    peer_ack_pending = true;
    peer_ack_request = 41U;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_OK)
        || (s_pm_psram_stack_stable_state != PM_PSRAM_STACK_ACTIVE)
        || (send_calls != 0U)) {{
        failures++;
    }}

    /* A peer transaction converging elsewhere is adopted, then overridden by
     * this caller's own transaction. */
    reset_transaction();
    s_pm_psram_stack_state = PM_PSRAM_STACK_WAITING;
    s_pm_psram_stack_request_id = 51U;
    s_pm_psram_stack_request_generation = 51U;
    s_pm_psram_stack_request_sleep_state = 1U;
    peer_ack_pending = true;
    peer_ack_request = 51U;
    deliver_ack = true;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_OK)
        || (s_pm_psram_stack_stable_state != PM_PSRAM_STACK_ACTIVE)
        || (send_calls != 1U)) {{
        failures++;
    }}

    /* A peer transaction that never settles bounds the wait and fails. */
    reset_transaction();
    s_pm_psram_stack_state = PM_PSRAM_STACK_WAITING;
    s_pm_psram_stack_request_id = 61U;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_FAIL)
        || (send_calls != 0U)) {{
        failures++;
    }}

    /* Quarantined and locked-out proxies fail fast without waiting. */
    reset_transaction();
    s_pm_psram_stack_state = PM_PSRAM_STACK_TIMED_OUT;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_FAIL)
        || (send_calls != 0U)) {{
        failures++;
    }}
    reset_transaction();
    s_pm_psram_stack_state = PM_PSRAM_STACK_WAITING;
    s_pm_psram_stack_locked_out = true;
    if ((bk_pm_psram_stack_vote_sleep_ctrl(0U) != BK_FAIL)
        || (send_calls != 0U)) {{
        failures++;
    }}

    printf("ap_feature_on_failures=%u\\n", failures);
    return failures == 0U ? 0 : 1;
}}
"""


def cp_harness(case_body):
    return f"""
#include <stdint.h>
#include <stdio.h>

typedef int bk_err_t;
#define BK_OK 0
#define BK_FAIL (-1)
#define PM_SLEEP_MODULE_NAME_PSRAM 7U
#define PM_SLEEP_CTRL_CMD 3U
#define PM_PSRAM_STACK_SLEEP_CTRL_CMD 0x11U
typedef struct {{
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
}} message_t;
static message_t msg;
static char events[16];
static unsigned event_count;
static uint32_t ack_cmd;
static uint32_t ack_param1;
static uint32_t ack_param2;
static uint32_t ack_param3;
static bk_err_t bk_pm_module_vote_sleep_ctrl(
    uint32_t module, uint32_t state, uint32_t time)
{{
    (void)module; (void)state; (void)time;
    events[event_count++] = 'V';
    return -7;
}}
static bk_err_t bk_pm_cp0_response_cp1(
    uint32_t cmd, uint32_t p1, uint32_t p2, uint32_t p3)
{{
    events[event_count++] = 'A';
    ack_cmd = cmd; ack_param1 = p1; ack_param2 = p2; ack_param3 = p3;
    return BK_OK;
}}
static void run_provider(void)
{{
    bk_err_t ret = BK_OK;
    (void)ret;
{case_body}
}}
int main(void)
{{
    unsigned failures = 0U;
    msg.param1 = PM_SLEEP_MODULE_NAME_PSRAM;
    msg.param2 = 1U;
    msg.param3 = 0x1234U;
    run_provider();
    if ((event_count != 2U) || (events[0] != 'V') || (events[1] != 'A')) {{
        failures++;
    }}
    if (ack_cmd != PM_PSRAM_STACK_SLEEP_CTRL_CMD) {{
        failures++;
    }}
    if (!((ack_param1 == (uint32_t)-7 || ack_param2 == (uint32_t)-7
            || ack_param3 == (uint32_t)-7)
        && (ack_param1 == msg.param3 || ack_param2 == msg.param3
            || ack_param3 == msg.param3))) {{
        failures++;
    }}
    const message_t invalid[] = {{
        {{6U, 0U, 1U}},
        {{PM_SLEEP_MODULE_NAME_PSRAM, 2U, 1U}},
        {{PM_SLEEP_MODULE_NAME_PSRAM, 0U, 0U}},
    }};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {{
        msg = invalid[i];
        event_count = 0U;
        ack_param2 = BK_OK;
        run_provider();
        if ((event_count != 1U) || (events[0] != 'A')
            || ((int32_t)ack_param2 != BK_FAIL)) {{
            failures++;
        }}
    }}
    printf("cp_failures=%u\\n", failures);
    return failures == 0U ? 0 : 1;
}}
"""


def main():
    try:
        ap = AP_SOURCE.read_text(encoding="utf-8")
        cp = CP_SOURCE.read_text(encoding="utf-8")
    except OSError as error:
        print(f"PM-05C:PROVIDER-ACTUAL:HARNESS-FAIL {error}", file=sys.stderr)
        return 2
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
    cp_case = extract_case_body(cp, "LOW_PWR_CORE_PSRAM_STACK_SLEEP")
    if not ap_functions or not cp_case:
        print(
            "PM-05C:PROVIDER-ACTUAL:HARNESS-FAIL production source missing",
            file=sys.stderr,
        )
        return 2
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    ap_result, ap_output = compile_and_run(
        "pm05c_ap_provider_actual", ap_harness(ap_functions)
    )
    ap_on_result, ap_on_output = compile_and_run(
        "pm05c_ap_provider_actual_feature_on",
        ap_feature_on_harness(ap_functions),
        defines=("CONFIG_CACHEABLE_PSRAM_TASK_STACK=1",),
    )
    cp_result, cp_output = compile_and_run(
        "pm05c_cp_provider_actual", cp_harness(cp_case)
    )
    sys.stdout.write(ap_output)
    sys.stdout.write(ap_on_output)
    sys.stdout.write(cp_output)
    if ap_result != 0 or ap_on_result != 0 or cp_result != 0:
        print(
            f"PM-05C:PROVIDER-ACTUAL:FAIL ap={ap_result} "
            f"ap_feature_on={ap_on_result} cp={cp_result}"
        )
        return 1
    print("PM-05C:PROVIDER-ACTUAL:PASS ap=0 ap_feature_on=0 cp=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
