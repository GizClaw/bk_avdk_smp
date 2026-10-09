#!/usr/bin/env python3

import re
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
KERNEL_DIR = TEST_DIR.parents[3]
SDK_DIR = next(parent.parent for parent in TEST_DIR.parents if parent.name == "ap")
FACADE = SDK_DIR / "ap/components/bk_rtos/freertos/v10/psram_stack_pm.c"
ADDITIONS = KERNEL_DIR / "include/additions/freertos_tasks_c_additions.h"
TASKS = KERNEL_DIR / "tasks.c"
AP_PROVIDER = SDK_DIR / "ap/middleware/driver/pwr_clk/pwr_clk.c"
CP_HANDLER = SDK_DIR / "cp/middleware/driver/pwr_clk/low_pwr_core.c"
AP_HEADER = SDK_DIR / "ap/include/driver/pwr_clk.h"
CP_HEADER = SDK_DIR / "cp/include/driver/pwr_clk.h"
ALLOCATOR_HEADER = KERNEL_DIR / "portable/MemMang/psram_stack_allocator.h"
MAKEFILE = TEST_DIR / "Makefile"


def extract_function(text, name):
    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{}}]*\)[^;{{}}]*\{{", text)
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
                return text[match.start() : position + 1]
    return ""


def extract_case(text, marker):
    start = text.find(f"case {marker}:")
    if start < 0:
        return ""
    end = text.find("break;", start)
    return text[start : end + len("break;")] if end >= 0 else ""


def has_order(text, patterns):
    position = 0
    for pattern in patterns:
        match = re.search(pattern, text[position:], re.DOTALL)
        if match is None:
            return False
        position += match.end()
    return True


class Contract:
    def __init__(self):
        self.checks = 0
        self.missing = []

    def require(self, condition, name):
        self.checks += 1
        if not condition:
            self.missing.append(name)
            print(f"PM-05C:SOURCE:MISSING {name}", file=sys.stderr)


def main():
    try:
        facade = FACADE.read_text(encoding="utf-8")
        additions = ADDITIONS.read_text(encoding="utf-8")
        tasks = TASKS.read_text(encoding="utf-8")
        ap_provider = AP_PROVIDER.read_text(encoding="utf-8")
        cp_handler = CP_HANDLER.read_text(encoding="utf-8")
        ap_header = AP_HEADER.read_text(encoding="utf-8")
        cp_header = CP_HEADER.read_text(encoding="utf-8")
        allocator_header = ALLOCATOR_HEADER.read_text(encoding="utf-8")
        makefile = MAKEFILE.read_text(encoding="utf-8")
    except OSError as error:
        print(f"PM-05C:SOURCE:HARNESS-FAIL {error}", file=sys.stderr)
        return 2

    contract = Contract()
    create = extract_function(additions, "xTaskCreateInPsram")
    rollback = extract_function(
        additions, "prvFreeUnpublishedCacheablePsramTask"
    )
    delete = extract_function(tasks, "prvDeleteTCB")
    delete_api = extract_function(tasks, "vTaskDelete")
    ap_generic_ack = extract_case(ap_provider, "PM_SLEEP_CTRL_CMD")
    ap_stack_ack = extract_case(ap_provider, "PM_PSRAM_STACK_SLEEP_CTRL_CMD")
    cp_transaction = extract_case(cp_handler, "LOW_PWR_CORE_PSRAM_STACK_SLEEP")

    contract.require(
        not any(
            token in facade + additions + tasks
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
        "no-task-owned-lifecycle-mutex-or-participant",
    )
    contract.require(
        has_order(
            create,
            [
                r"configPSRAM_STACK_PM_VOTE_ACTIVE\s*\(",
                r"prvAddNewTaskToReadyList\s*\(",
            ],
        ),
        "create-confirms-active-before-publication-without-task-owner",
    )
    contract.require(
        "LIFECYCLE_OWNER" not in delete_api
        and "xSemaphoreGetMutexHolder" not in delete_api,
        "external-delete-has-no-task-owned-participant-recovery",
    )
    contract.require(
        rollback.count("configDEINIT_TLS_BLOCK") == 1
        and "portCLEAN_UP_TCB" not in rollback,
        "rollback-deinits-tls-once-without-port-cleanup",
    )
    contract.require(
        has_order(
            delete,
            [
                r"portCLEAN_UP_TCB\s*\(",
                r"configDEINIT_TLS_BLOCK\s*\(",
                r"psram_stack_free_exact_with_transition\s*\(",
                r"configPSRAM_STACK_PM_VOTE_RELEASED\s*\(",
                r"vPortFree\s*\(\s*pxTCB\s*\)",
            ],
        ),
        "delete-physical-cleanup-relies-on-allocator-and-shared-state",
    )
    contract.require(
        "PM_PSRAM_STACK_SLEEP_CTRL_CMD" in ap_header
        and "PM_PSRAM_STACK_SLEEP_CTRL_CMD" in cp_header
        and "PM_PSRAM_STACK_SLEEP_CTRL_CMD" in ap_provider
        and "PM_SLEEP_CTRL_CMD" in ap_provider,
        "dedicated-psram-stack-sleep-transaction-command",
    )
    contract.require(
        bool(cp_transaction)
        and has_order(
            cp_transaction,
            [
                r"ret\s*=\s*bk_pm_module_vote_sleep_ctrl\s*\(",
                r"bk_pm_cp0_response_cp1\s*\("
                r"\s*PM_PSRAM_STACK_SLEEP_CTRL_CMD",
                r"(?:request_id|msg\.param3)",
                r"ret",
            ],
        ),
        "cp-acks-request-id-and-actual-ret-after-real-vote",
    )
    contract.require(
        all(
            token in ap_provider
            for token in (
                "PM_PSRAM_STACK_PREPARING",
                "PM_PSRAM_STACK_WAITING",
                "PM_PSRAM_STACK_TIMED_OUT",
                "PM_PSRAM_STACK_DONE",
            )
        ),
        "ap-dedicated-atomic-transaction-states",
    )
    contract.require(
        bool(ap_generic_ack)
        and bool(ap_stack_ack)
        and "pm_psram_stack_ack" not in ap_generic_ack
        and "pm_psram_stack_ack" in ap_stack_ack,
        "generic-sleep-ack-cannot-complete-psram-stack-transaction",
    )
    contract.require(
        all(
            token in ap_provider
            for token in (
                "late",
                "TIMED_OUT",
                "request_id",
                "BK_FAIL",
            )
        ),
        "timeout-late-ack-concurrent-and-nack-fail-closed",
    )
    contract.require(
        re.search(
            r"static\s+uint32_t\s+s_pm_psram_stack_request_generation",
            ap_provider,
        )
        is not None
        and "s_pm_psram_stack_request_generation == UINT32_MAX" in ap_provider
        and "s_pm_psram_stack_locked_out" in ap_provider
        and re.search(r"uint64_t\s+generation\s*;", allocator_header) is not None,
        "wire-generation-is-uint32-and-exhaustion-locks-out",
    )
    contract.require(
        all(
            token in makefile
            for token in (
                "pm05c-facade-actual",
                "pm05c-provider-actual",
                "pm05c-source-contract",
                "pm05c-layout-arm",
            )
        ),
        "serial-verify-includes-all-pm05c-gates",
    )

    if contract.missing:
        print(
            f"PM-05C:SOURCE:FAIL checks={contract.checks} "
            f"missing={len(contract.missing)}",
            file=sys.stderr,
        )
        return 1
    print(f"PM-05C:SOURCE:PASS checks={contract.checks} missing=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
