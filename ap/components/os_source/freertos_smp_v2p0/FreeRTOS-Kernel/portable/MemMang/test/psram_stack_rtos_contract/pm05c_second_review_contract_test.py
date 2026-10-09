#!/usr/bin/env python3
import re
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
KERNEL = HERE.parents[3]
SDK = HERE.parents[8]
TASKS = KERNEL / "tasks.c"
ADDITIONS = KERNEL / "include/additions/freertos_tasks_c_additions.h"
HEAP = KERNEL / "portable/MemMang/heap_4.c"
ALLOCATOR_HEADER = KERNEL / "portable/MemMang/psram_stack_allocator.h"
FACADE = SDK / "ap/components/bk_rtos/freertos/v10/psram_stack_pm.c"
AP_PM = SDK / "ap/middleware/driver/pwr_clk/pwr_clk.c"
CP_CORE = SDK / "cp/middleware/driver/pwr_clk/low_pwr_core.c"
CP_PM = SDK / "cp/components/bk_pm/pm.c"
AP_HEADER = SDK / "ap/include/driver/pwr_clk.h"
CP_HEADER = SDK / "cp/include/driver/pwr_clk.h"
KCONFIG = SDK / "ap/components/bk_rtos/Kconfig"
AP_PROJECT_CONFIG = SDK / "projects/test/platform/ap/config/bk7258_ap/config"
CP_PROJECT_CONFIG = SDK / "projects/test/platform/cp/config/bk7258/config"
MAKEFILE = HERE / "Makefile"


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


def extract_case(text, marker):
    match = re.search(rf"case\s+{re.escape(marker)}\s*:", text)
    if match is None:
        return ""
    next_case = re.search(r"\n\s*(?:case\s+|default\s*:)", text[match.end() :])
    end = len(text) if next_case is None else match.end() + next_case.start()
    return text[match.start() : end]


def ordered(text, patterns):
    position = 0
    for pattern in patterns:
        match = re.search(pattern, text[position:], re.S)
        if match is None:
            return False
        position += match.end()
    return True


def main():
    files = {}
    try:
        for key, path in (
            ("tasks", TASKS),
            ("additions", ADDITIONS),
            ("heap", HEAP),
            ("allocator_header", ALLOCATOR_HEADER),
            ("facade", FACADE),
            ("ap", AP_PM),
            ("cp_core", CP_CORE),
            ("cp_pm", CP_PM),
            ("ap_header", AP_HEADER),
            ("cp_header", CP_HEADER),
            ("kconfig", KCONFIG),
            ("ap_project_config", AP_PROJECT_CONFIG),
            ("cp_project_config", CP_PROJECT_CONFIG),
            ("makefile", MAKEFILE),
        ):
            files[key] = path.read_text(encoding="utf-8")
    except OSError as error:
        print(f"PM-05C:REVIEW2:HARNESS-FAIL {error}", file=sys.stderr)
        return 2

    delete_api = extract_function(files["tasks"], "vTaskDelete")
    delete_tcb = extract_function(files["tasks"], "prvDeleteTCB")
    create_api = extract_function(files["additions"], "xTaskCreateInPsram")
    alloc = extract_function(files["heap"], "psram_stack_alloc_with_transition")
    prepare_power = extract_function(
        files["heap"], "psram_stack_prepare_power_locked"
    )
    ap_vote = extract_function(files["ap"], "bk_pm_psram_stack_vote_sleep_ctrl")
    ap_ack = extract_function(files["ap"], "pm_psram_stack_ack")
    cp_stack = extract_case(files["cp_core"], "LOW_PWR_CORE_PSRAM_STACK_SLEEP")
    destructive = extract_function(files["cp_pm"], "bk_pm_sleep_mode_set")
    lowvol_set = extract_function(files["cp_pm"], "pm_low_voltage_resource_set")
    lowvol_process = extract_function(files["cp_pm"], "pm_low_voltage_process")
    direct_off = extract_function(
        files["cp_pm"], "pm_psram_malloc_state_and_power_ctrl"
    )
    ap_load = extract_function(files["ap_header"], "pm_shared_u64_load")
    ap_store = extract_function(files["ap_header"], "pm_shared_u64_store")
    ap_bit_set = extract_function(files["ap_header"], "pm_shared_u64_bit_set")
    ap_bit_clear = extract_function(files["ap_header"], "pm_shared_u64_bit_clear")
    legacy = files["tasks"] + files["additions"] + files["facade"]
    checks = {
        "no-task-owned-mutex-or-participants": not any(
            token in legacy
            for token in (
                "StaticSemaphore_t",
                "xSemaphoreCreateMutexStatic",
                "xSemaphoreGetMutexHolder",
                "LIFECYCLE_OWNER",
                "LIFECYCLE_TRY_LOCK",
                "prvPsramStackLifecycleLock",
                "participant",
            )
        ),
        "heap-lock-linearizes-first-live-leader": ordered(
            alloc + files["heap"],
            [
                r"HeapEnterCritical",
                r"s_psram_stack_live_count\+\+",
                r"PSRAM_STACK_PM_TRANSITION_FIRST_LIVE",
                r"HeapExitCritical",
            ],
        ),
        "second-create-active-before-publication": re.search(
            r"(?:ACTIVE_CONFIRMED|PM_IS_ACTIVE|STATE_ACTIVE)"
            r"[\s\S]*prvAddNewTaskToReadyList",
            create_api,
        )
        is not None,
        "pending-last-free-hold-blocks-new-allocation": re.search(
            r"if\s*\(\s*s_psram_stack_pm_hold_generation\s*!=\s*0U\s*\)"
            r"\s*\{\s*return\s+PSRAM_POWER_GUARD_BUSY",
            prepare_power,
        )
        is not None,
        "allocator-exposes-uint64-claim-commit-abort": all(
            re.search(pattern, files["allocator_header"]) is not None
            for pattern in (
                r"hold_release_claim\s*\(\s*uint64_t\s*\*",
                r"hold_release_commit\s*\(\s*uint64_t",
                r"hold_release_abort\s*\(\s*uint64_t",
            )
        ),
        "send-failure-restores-idle-and-is-retryable": re.search(
            r"pm_cp1_mailbox_send_data[\s\S]*?!=\s*BK_OK[\s\S]*?"
            r"PM_PSRAM_STACK_IDLE",
            ap_vote,
        )
        is not None
        and "PM_PSRAM_STACK_TIMED_OUT" not in ap_vote[
            ap_vote.find("pm_cp1_mailbox_send_data") : ap_vote.find("start =")
        ],
        "timeout-retains-quarantine": "pm_psram_stack_quarantine" in ap_vote
        and "PM_PSRAM_STACK_TIMED_OUT" in files["ap"],
        "shared-ack-state-is-globally-noncache": re.search(
            r"s_pm_psram_stack_(?:state|request_id|result)"
            r"[\s\S]{0,160}section\s*\(\s*\"[^\"]*(?:noncache|sram)",
            files["ap"],
            re.I,
        )
        is not None,
        "leader-teardown-only-converges-shared-state": not any(
            token in ap_vote + ap_ack
            for token in (
                "TaskHandle_t",
                "xTaskGetCurrentTaskHandle",
                "vTaskDelete",
                "lifecycle_owner",
                "participant",
            )
        ),
        "late-ack-never-commits-allocator-hold": (
            "psram_stack_pm_hold_release_commit" not in ap_ack
            and re.search(
                r"TIMED_OUT[\s\S]*?(?:IDLE|quarantine)", ap_ack, re.I
            )
            is not None
        ),
        "cp-invalid-request-nacks-without-vote": ordered(
            cp_stack,
            [
                r"PM_SLEEP_MODULE_NAME_PSRAM",
                r"(?:msg\.param2\s*>\s*1|sleep_state\s*>\s*1)",
                r"(?:request_id|msg\.param3)\s*(?:==|!=)\s*0",
                r"BK_FAIL",
                r"bk_pm_cp0_response_cp1",
            ],
        )
        and cp_stack.find("BK_FAIL") < cp_stack.find("bk_pm_module_vote_sleep_ctrl"),
        "shared-fixed64-has-per-field-version": all(
            re.search(
                r"typedef\s+struct\s*\{[\s\S]*uint32_t\s+version"
                r"[\s\S]*uint32_t\s+low[\s\S]*uint32_t\s+high"
                r"[\s\S]*\}\s*pm_shared_u64_words_t",
                files[key],
            )
            is not None
            for key in ("ap_header", "cp_header")
        ),
        "shared-fixed64-bounded-seqlock-fail-closed": all(
            all(
                token in files[key]
                for token in (
                    "PM_SHARED_U64_MAX_RETRIES",
                    "version",
                    "__ATOMIC_ACQUIRE",
                    "__ATOMIC_RELEASE",
                )
            )
            for key in ("ap_header", "cp_header")
        ),
        "shared-fixed64-reader-has-acquire-fence-or-dmb": (
            re.search(
                r"version[\s\S]*?(?:__atomic_thread_fence\s*\(\s*"
                r"__ATOMIC_ACQUIRE|__DMB\s*\(\s*\))[\s\S]*?(?:low|high)",
                ap_load,
            )
            is not None
        ),
        "shared-fixed64-payload-is-fully-atomic": all(
            "__atomic_" in body
            and re.search(r"\*word\s*(?:\|=|&=|\^=|=)", body) is None
            for body in (ap_store, ap_bit_set, ap_bit_clear)
        ),
        "shared-fixed64-writer-cas-bounded-wrap-failclosed": all(
            token in ap_store
            for token in (
                "__atomic_compare_exchange_n",
                "UINT32_MAX - 2U",
                "return false",
            )
        )
        and re.search(
            r"retry\s*<\s*(?:PM_SHARED_U64_MAX_RETRIES|32U)", ap_store
        )
        is not None,
        "destructive-modes-guard-before-side-effects": all(
            mode in destructive
            for mode in (
                "PM_MODE_DEEP_SLEEP",
                "PM_MODE_SUPER_DEEP_SLEEP",
                "PM_MODE_FORCE_DEEP_SLEEP",
            )
        )
        and ordered(
            destructive,
            [
                r"(?:PSRAM_STACK|psram_stack).*(?:live|vote|safe)",
                r"(?:return\s+BK_FAIL|BK_FAIL)",
                r"(?:s_pm_deepsleep_enter_cb_conf|s_pm_superdeep_enter_cb_conf)",
            ],
        ),
        "lowvol-caller-aborts-before-enter-on-unsafe": (
            re.search(
                r"pm_low_voltage_resource_set\s*\(\s*\)\s*"
                r"(?:!=\s*BK_OK|==\s*BK_FAIL|==\s*false)"
                r"[\s\S]*?return[\s\S]*?pm_enter_low_voltage",
                lowvol_process,
            )
            is not None
            and re.search(r"(?:bk_err_t|bool)\s+pm_low_voltage_resource_set", lowvol_set)
            is not None
        ),
        "direct-psram-off-has-final-guard": (
            direct_off.count("pm_psram_stack_destructive_is_safe") >= 2
            and direct_off.rfind("pm_psram_stack_destructive_is_safe")
            < direct_off.find("bk_pm_module_vote_psram_ctrl")
        ),
        "cp-guard-kconfig-default-off-project-paired-on": (
            re.search(
                r"config\s+CACHEABLE_PSRAM_TASK_STACK[\s\S]*?default\s+n",
                files["kconfig"],
            )
            is not None
            and "CONFIG_CACHEABLE_PSRAM_TASK_STACK=y"
            in files["ap_project_config"]
            and "CONFIG_CACHEABLE_PSRAM_TASK_STACK=y"
            in files["cp_project_config"]
        ),
        "facade-routes-through-dedicated-ap-transaction": (
            "bk_pm_psram_stack_vote_sleep_ctrl"
            in extract_function(files["facade"], "bk_rtos_psram_stack_pm_vote")
        ),
        "serial-verify-includes-second-review-gates": all(
            token in files["makefile"]
            for token in (
                "pm05c-review2-model",
                "pm05c-review2-contract",
                "pm05c-cp-destructive-actual",
                "pm05c-integration-actual",
            )
        ),
    }
    for name, passed in checks.items():
        if not passed:
            print(f"PM-05C:REVIEW2:MISSING {name}", file=sys.stderr)
    missing = sum(not passed for passed in checks.values())
    print(
        f"PM-05C:REVIEW2:{'PASS' if missing == 0 else 'FAIL'} "
        f"checks={len(checks)} missing={missing}"
    )
    return 0 if missing == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
