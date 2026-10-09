#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
SDK = HERE.parents[8]
SOURCE = SDK / "cp/components/bk_pm/pm.c"
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


def harness(function, feature_on):
    return f"""
#include <stdint.h>
#include <stdio.h>
typedef int bk_err_t;
typedef int pm_sleep_mode_e;
#define BK_OK 0
#define BK_FAIL (-1)
#define CONFIG_PM_SUPER_DEEP_SLEEP 1
#define CONFIG_CACHEABLE_PSRAM_TASK_STACK {feature_on}
#define PM_MODE_NORMAL_SLEEP 0
#define PM_MODE_LOW_VOLTAGE 1
#define PM_MODE_DEEP_SLEEP 2
#define PM_MODE_SUPER_DEEP_SLEEP 3
#define PM_MODE_FORCE_DEEP_SLEEP 4
#define PM_MODE_DEFAULT 5
#define PM_DEEPSLEEP_CB_SIZE 1
#define PM_SUPERDEEP_CB_SIZE 1
#define PM_POWER_MODULE_NAME_AUDP 1
#define PM_POWER_MODULE_NAME_VIDP 2
#define PM_POWER_MODULE_NAME_BTSP 3
#define PM_POWER_MODULE_NAME_WIFIP_MAC 4
typedef void (*callback_t)(uint32_t, void *);
typedef struct {{ callback_t cb; void *args; }} callback_cfg_t;
typedef struct {{ callback_cfg_t cfg; uint32_t id; }} callback_entry_t;
static callback_entry_t s_pm_deepsleep_enter_cb_conf[1];
static callback_entry_t s_pm_superdeep_enter_cb_conf[1];
static pm_sleep_mode_e s_pm_sleep_mode;
uint32_t test_stack_live_count;
uint32_t test_psram_vote_active;
#define FIXED_ADDR_PSRAM_USDE_COUNT test_stack_live_count
#define PM_SLEEP_MODULE_NAME_PSRAM 7U
static uint64_t s_pm_sleeped_modules;
static uint32_t callback_calls;
static uint32_t register_writes;
static void callback(uint32_t value, void *arg)
{{ (void)value; (void)arg; callback_calls++; }}
static bk_err_t pm_check_multimedia_pwr_bus_state(void) {{ return BK_OK; }}
static void bk_pm_clear_deep_sleep_modules_config(uint32_t module)
{{ (void)module; register_writes++; }}
static bk_err_t pm_force_analog_close(void)
{{ register_writes++; return BK_OK; }}
{function}
static void reset_counts(void)
{{
    callback_calls = 0U;
    register_writes = 0U;
    s_pm_deepsleep_enter_cb_conf[0].cfg.cb = callback;
    s_pm_superdeep_enter_cb_conf[0].cfg.cb = callback;
}}
int main(void)
{{
    unsigned failures = 0U;
    (void)s_pm_sleeped_modules;
#if CONFIG_CACHEABLE_PSRAM_TASK_STACK
    const int modes[] = {{
        PM_MODE_DEEP_SLEEP, PM_MODE_SUPER_DEEP_SLEEP,
        PM_MODE_FORCE_DEEP_SLEEP
    }};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {{
        reset_counts();
        test_stack_live_count = 1U;
    test_psram_vote_active = 0U;
    s_pm_sleeped_modules = 1ULL << PM_SLEEP_MODULE_NAME_PSRAM;
        if ((bk_pm_sleep_mode_set(modes[i]) != BK_FAIL)
            || callback_calls != 0U || register_writes != 0U) {{
            failures++;
        }}
    }}
    reset_counts();
    test_stack_live_count = 0U;
    test_psram_vote_active = 1U;
    s_pm_sleeped_modules = 0U;
    if ((bk_pm_sleep_mode_set(PM_MODE_DEEP_SLEEP) != BK_FAIL)
        || callback_calls != 0U || register_writes != 0U) {{
        failures++;
    }}
#else
    reset_counts();
    if ((bk_pm_sleep_mode_set(PM_MODE_DEEP_SLEEP) != BK_OK)
        || callback_calls != 1U) {{
        failures++;
    }}
#endif
    printf("feature={feature_on} failures=%u\\n", failures);
    return failures == 0U ? 0 : 1;
}}
"""


def direct_off_harness(safe_function, direct_function):
    return f"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int bk_err_t;
#define BK_OK 0
#define BK_FAIL (-1)
#define CONFIG_CACHEABLE_PSRAM_TASK_STACK 1
#define CONFIG_PM_PSRAM_FORCE_ON 0
#define CONFIG_PSRAM_AS_SYS_MEMORY 1
#define CONFIG_PM_AP_POWERDOWN_WHEN_LV 1
#define CONFIG_PM_PSRAM_DEBUG 0
#define CONFIG_CPU_CNT 1
#define PM_SLEEP_MODULE_NAME_PSRAM 7U
#define PM_POWER_PSRAM_MODULE_NAME_AS_MEM 1U
#define PM_POWER_MODULE_STATE_OFF 0U
#define PM_POWER_SUB_MODULE_NAME_AHBP_PSRAM 2U
static uint32_t test_shared_used_count;
#define FIXED_ADDR_PSRAM_USDE_COUNT test_shared_used_count
static uint64_t s_pm_sleeped_modules;
static unsigned off_calls;
static uint32_t bk_psram_heap_get_used_count(void)
{{
    test_shared_used_count = 1U;
    return 0U;
}}
static bk_err_t bk_pm_module_vote_psram_ctrl(uint32_t module, uint32_t state)
{{ (void)module; (void)state; off_calls++; return BK_OK; }}
static bk_err_t bk_pm_module_vote_power_ctrl(uint32_t module, uint32_t state)
{{ (void)module; (void)state; off_calls++; return BK_OK; }}
{safe_function}
{direct_function}
int main(void)
{{
    test_shared_used_count = 0U;
    s_pm_sleeped_modules = 1ULL << PM_SLEEP_MODULE_NAME_PSRAM;
    const bk_err_t result = pm_psram_malloc_state_and_power_ctrl();
    printf("direct_off_result=%d off_calls=%u\\n", result, off_calls);
    return (result == BK_FAIL && off_calls == 0U) ? 0 : 1;
}}
"""


def compile_run(function, feature_on):
    BUILD.mkdir(parents=True, exist_ok=True)
    source = BUILD / f"pm05c_cp_destructive_{feature_on}.c"
    binary = BUILD / f"pm05c_cp_destructive_{feature_on}"
    source.write_text(harness(function, feature_on), encoding="utf-8")
    compile_result = subprocess.run(
        [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(source), "-o", str(binary),
        ],
        text=True,
        capture_output=True,
    )
    if compile_result.returncode != 0:
        return compile_result.returncode, compile_result.stdout + compile_result.stderr
    run_result = subprocess.run([str(binary)], text=True, capture_output=True)
    return run_result.returncode, run_result.stdout + run_result.stderr


def compile_run_direct_off(safe_function, direct_function):
    BUILD.mkdir(parents=True, exist_ok=True)
    source = BUILD / "pm05c_cp_direct_off.c"
    binary = BUILD / "pm05c_cp_direct_off"
    source.write_text(
        direct_off_harness(safe_function, direct_function), encoding="utf-8"
    )
    compile_result = subprocess.run(
        [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(source), "-o", str(binary),
        ],
        text=True,
        capture_output=True,
    )
    if compile_result.returncode != 0:
        return compile_result.returncode, compile_result.stdout + compile_result.stderr
    run_result = subprocess.run([str(binary)], text=True, capture_output=True)
    return run_result.returncode, run_result.stdout + run_result.stderr


def main():
    try:
        text = SOURCE.read_text(encoding="utf-8")
    except OSError as error:
        print(f"PM-05C:CP-DESTRUCTIVE:HARNESS-FAIL {error}", file=sys.stderr)
        return 2
    function = extract_function(text, "bk_pm_sleep_mode_set")
    safe_function = extract_function(text, "pm_psram_stack_destructive_is_safe")
    direct_function = extract_function(text, "pm_psram_malloc_state_and_power_ctrl")
    lowvol_set = extract_function(text, "pm_low_voltage_resource_set")
    lowvol_process = extract_function(text, "pm_low_voltage_process")
    if not function:
        print("PM-05C:CP-DESTRUCTIVE:MISSING production-function", file=sys.stderr)
        return 1
    failures = 0
    for feature_on in (1, 0):
        result, output = compile_run(function, feature_on)
        sys.stdout.write(output)
        failures += result != 0
    result, output = compile_run_direct_off(safe_function, direct_function)
    sys.stdout.write(output)
    failures += result != 0
    lowvol_guarded = (
        re.search(
            r"pm_low_voltage_resource_set\s*\(\s*\)\s*"
            r"(?:!=\s*BK_OK|==\s*BK_FAIL|==\s*false)"
            r"[\s\S]*?return[\s\S]*?pm_enter_low_voltage",
            lowvol_process,
        )
        is not None
        and re.search(r"(?:bk_err_t|bool)\s+pm_low_voltage_resource_set", lowvol_set)
        is not None
    )
    if not lowvol_guarded:
        print(
            "PM-05C:CP-DESTRUCTIVE:MISSING "
            "actual-lowvol-caller-aborts-before-enter",
            file=sys.stderr,
        )
        failures += 1
    print(
        f"PM-05C:CP-DESTRUCTIVE:{'PASS' if failures == 0 else 'FAIL'} "
        f"variants=4 failures={failures}"
    )
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
