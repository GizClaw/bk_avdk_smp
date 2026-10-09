#!/usr/bin/env python3

import re
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
KERNEL_DIR = TEST_DIR.parents[3]
SDK_DIR = next(parent.parent for parent in TEST_DIR.parents if parent.name == "ap")
ADDITIONS = KERNEL_DIR / "include/additions/freertos_tasks_c_additions.h"
TASKS = KERNEL_DIR / "tasks.c"
ALLOCATOR_HEADER = KERNEL_DIR / "portable/MemMang/psram_stack_allocator.h"
FREERTOS_CONFIG = SDK_DIR / "ap/components/bk_rtos/freertos/FreeRTOSConfig.h"
RTOS_DIR = SDK_DIR / "ap/components/bk_rtos/freertos/v10"
FACADE_SOURCE = RTOS_DIR / "psram_stack_pm.c"
FACADE_HEADER = RTOS_DIR / "psram_stack_pm.h"
ACTUAL_TEST = TEST_DIR / "pm_lifecycle_actual_test.c"
MAKEFILE = TEST_DIR / "Makefile"


def extract_function(text, name):
    match = re.search(
        rf"\b{re.escape(name)}\s*\([^;{{}}]*\)[^;{{}}]*\{{",
        text,
        re.DOTALL,
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
                return text[match.start() : position + 1]
    return ""


def has_order(text, patterns):
    position = 0
    for pattern in patterns:
        match = re.search(pattern, text[position:], re.DOTALL)
        if match is None:
            return False
        position += match.end()
    return True


def function_names(text):
    return list(
        dict.fromkeys(
            re.findall(
                r"(?m)^[ \t]*(?:static[ \t]+)?"
                r"[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*]+"
                r"([A-Za-z_][A-Za-z0-9_]*)\s*"
                r"\([^;{}]*\)[^;{}]*\{",
                text,
            )
        )
    )


class Contract:
    def __init__(self):
        self.checks = 0
        self.missing = []

    def require(self, condition, name):
        self.checks += 1
        if not condition:
            self.missing.append(name)
            print(f"PM-05B:SOURCE:MISSING {name}", file=sys.stderr)


def main():
    try:
        additions = ADDITIONS.read_text(encoding="utf-8")
        tasks = TASKS.read_text(encoding="utf-8")
        allocator_header = ALLOCATOR_HEADER.read_text(encoding="utf-8")
        config = FREERTOS_CONFIG.read_text(encoding="utf-8")
        actual_test = ACTUAL_TEST.read_text(encoding="utf-8")
        makefile = MAKEFILE.read_text(encoding="utf-8")
        facade_source = (
            FACADE_SOURCE.read_text(encoding="utf-8")
            if FACADE_SOURCE.is_file()
            else ""
        )
        facade_header = (
            FACADE_HEADER.read_text(encoding="utf-8")
            if FACADE_HEADER.is_file()
            else ""
        )
        rtos_sources = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(RTOS_DIR.glob("*.[ch]"))
        )
    except OSError as error:
        print(f"PM-05B:SOURCE:HARNESS-FAIL {error}", file=sys.stderr)
        return 2

    contract = Contract()
    create = extract_function(additions, "xTaskCreateInPsram")
    allocate = extract_function(additions, "prvAllocateCacheablePsramTask")
    unpublished_free = extract_function(
        additions, "prvFreeUnpublishedCacheablePsramTask"
    )
    delete = extract_function(tasks, "prvDeleteTCB")
    facade_names = [
        name
        for name in function_names(rtos_sources)
        if "PM_SLEEP_MODULE_NAME_PSRAM"
        in extract_function(rtos_sources, name)
        and "bk_pm_module_vote_sleep_ctrl"
        in extract_function(rtos_sources, name)
    ]
    facade = (
        extract_function(rtos_sources, facade_names[0])
        if len(facade_names) == 1
        else ""
    )

    contract.require(
        bool(facade_source) and bool(facade_header),
        "real-psram-stack-pm-source-and-private-header",
    )
    contract.require(
        re.search(r"uint64_t\s+generation\s*;", allocator_header) is not None
        and re.search(
            r"psram_stack_pm_hold_release_commit\s*\(\s*uint64_t",
            allocator_header,
        )
        is not None,
        "allocator-generation-token-api-is-uint64",
    )
    contract.require(
        "psram_stack_alloc_with_transition(" in allocate
        and "PSRAM_STACK_PM_TRANSITION_FIRST_LIVE" in additions,
        "xTaskCreateInPsram-consumes-first-live-transition",
    )
    contract.require(
        len(facade_names) == 1,
        "single-private-bk-rtos-psram-sleep-vote-facade",
    )
    contract.require(
        bool(facade)
        and re.search(
            r"bk_pm_module_vote_sleep_ctrl\s*\(\s*"
            r"PM_SLEEP_MODULE_NAME_PSRAM\s*,\s*(?:sleep_state|state)"
            r"\s*,\s*0\s*\)",
            facade,
        )
        is not None
        and re.search(r"(?:sleep_state|state)\s*>\s*1", facade) is not None,
        "facade-maps-only-active-zero-and-released-one",
    )
    contract.require(
        bool(facade)
        and re.search(r"(?:InIsr|interrupt_context)", facade) is not None
        and re.search(r"(?:irq_disabled|interrupt.*disabled)", facade) is not None
        and all(
            token not in facade
            for token in (
                "xKernelLock",
                "HeapEnterCritical",
                "mailbox",
                "PM_CTRL_",
            )
        ),
        "facade-task-irq-enabled-and-lock-free-without-wire-change",
    )
    contract.require(
        bool(facade_source)
        and not any(
            token in facade_source + additions + tasks
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
        all(
            re.search(pattern, allocator_header + facade_header) is not None
            for pattern in (
                r"resync_claim\s*\([^)]*uint64_t",
                r"resync_commit\s*\([^)]*uint64_t",
                r"resync_abort\s*\([^)]*uint64_t",
            )
        ),
        "shared-resync-claim-commit-abort-is-uint64",
    )
    contract.require(
        all(
            token in config
            for token in (
                "configPSRAM_STACK_PM_ENSURE_READY",
                "configPSRAM_STACK_PM_VOTE_ACTIVE",
                "configPSRAM_STACK_PM_VOTE_RELEASED",
            )
        ),
        "freertos-config-exposes-private-pm-facade-hooks",
    )
    contract.require(
        has_order(
            create,
            [
                r"configPSRAM_STACK_PM_ENSURE_READY\s*\(",
                r"prvAllocateCacheablePsramTask\s*\(",
            ],
        ),
        "lazy-resync-completes-before-first-allocation",
    )
    contract.require(
        has_order(
            rtos_sources,
            [
                r"configPSRAM_STACK_PM_VOTE_RELEASED|"
                r"bk_pm_module_vote_sleep_ctrl\s*\([^;]*,\s*1\s*,",
                r"(?:ready|resync_ready)\s*=\s*(?:true|1)",
            ],
        ),
        "lazy-ap-reset-resync-releases-baseline-before-ready",
    )
    contract.require(
        has_order(
            create,
            [
                r"taskEXIT_CRITICAL\s*\(\s*&xKernelLock\s*\)",
                r"configPSRAM_STACK_PM_VOTE_ACTIVE\s*\(",
                r"prvAddNewTaskToReadyList\s*\(",
            ],
        ),
        "first-live-active-ack-precedes-ready-publication-outside-lock",
    )
    contract.require(
        re.search(
            r"(?:ACTIVE_CONFIRMED|PM_IS_ACTIVE|STATE_ACTIVE)"
            r"[\s\S]*prvAddNewTaskToReadyList",
            create,
        )
        is not None,
        "second-create-confirms-active-before-publication",
    )
    contract.require(
        has_order(
            create,
            [
                r"configPSRAM_STACK_PM_VOTE_ACTIVE\s*\(",
                r"(?:!=\s*BK_OK|==\s*BK_OK)",
                r"prvFreeUnpublishedCacheablePsramTask\s*\(",
            ],
        )
        and "prvAddNewTaskToReadyList" not in create[
            create.find("configPSRAM_STACK_PM_VOTE_ACTIVE") :
            create.find("prvFreeUnpublishedCacheablePsramTask")
        ],
        "active-nack-timeout-never-publishes-and-exact-rolls-back",
    )
    contract.require(
        all(
            token in unpublished_free
            for token in (
                "psram_stack_free_exact_with_transition",
                "PSRAM_STACK_PM_TRANSITION_LAST_FREE",
                "configPSRAM_STACK_PM_VOTE_RELEASED",
                "psram_stack_pm_hold_release_commit",
            )
        ),
        "create-rollback-compensates-last-free-token-before-commit",
    )
    contract.require(
        re.search(
            r"configDEINIT_TLS_BLOCK\s*\(\s*pxTCB->xTLSBlock\s*\)",
            unpublished_free,
        )
        is not None
        and unpublished_free.find("configDEINIT_TLS_BLOCK")
        < unpublished_free.find("vPortFree"),
        "unpublished-rollback-deinitializes-tls-before-tcb-free",
    )
    contract.require(
        "configPSRAM_STACK_FATAL_RESET" in unpublished_free
        or "prvPsramStackKernelFatal" in unpublished_free,
        "rollback-exact-free-failure-enters-wdt-fatal",
    )
    contract.require(
        re.search(r"(?:lockout|locked_out)", rtos_sources) is not None
        and re.search(
            r"(?:lockout|locked_out)\s*=\s*(?:true|1)",
            rtos_sources,
        )
        is not None,
        "compensation-failure-retains-hold-and-locks-out-create",
    )
    contract.require(
        re.search(r"(?:atomic|compare_exchange|spinlock)", rtos_sources) is not None
        and re.search(r"(?:inflight|in_flight)", rtos_sources) is not None,
        "concurrent-first-live-has-single-facade-transaction",
    )
    contract.require(
        has_order(
            delete,
            [
                r"psram_stack_free_exact_with_transition\s*\(",
                r"PSRAM_STACK_PM_TRANSITION_LAST_FREE",
                r"configPSRAM_STACK_PM_VOTE_RELEASED\s*\(",
                r"psram_stack_pm_hold_release_commit\s*\(",
            ],
        ),
        "delete-last-free-release-ack-then-token-commit",
    )
    contract.require(
        re.search(r"(?:lockout|locked_out)", delete) is not None
        and has_order(
            delete,
            [
                r"psram_stack_free_exact_with_transition\s*\(",
                r"(?:lockout|locked_out)",
                r"vPortFree\s*\(\s*pxTCB\s*\)",
                r"return\s*;",
            ],
        )
        and delete.count("configDEINIT_TLS_BLOCK") == 1
        and re.search(
            r"configDEINIT_TLS_BLOCK\s*\(\s*pxTCB->xTLSBlock\s*\)",
            delete,
        )
        is not None,
        "delete-nack-retains-hold-lockout-and-frees-tcb-once",
    )
    contract.require(
        all(
            token in actual_test
            for token in (
                "PSRAM_STACK_PM_TRANSITION_FIRST_LIVE",
                "PSRAM_STACK_PM_TRANSITION_LAST_FREE",
                "pending last-free hold blocks new allocation without mutation",
            )
        ),
        "actual-test-directly-exercises-allocator-transition-generation",
    )
    contract.require(
        "PM_LIFECYCLE_ACTUAL_TARGET" in makefile
        and "PM_LIFECYCLE_MODEL_TARGET" in makefile
        and "pm-facade-actual" in makefile
        and "pm-tls-arm" in makefile
        and "pm-lifecycle-source-contract" in makefile,
        "serial-verify-includes-actual-model-source-pm-gates",
    )
    contract.require(
        "PM05A_TEST_DIR" in makefile and "psram_stack_contract" in makefile,
        "official-verify-includes-pm05a-regression",
    )
    contract.require(
        all(
            len(extract_function(rtos_sources, name).splitlines()) <= 50
            for name in function_names(rtos_sources)
            if "psram_stack_pm" in name
            or "PM_SLEEP_MODULE_NAME_PSRAM"
            in extract_function(rtos_sources, name)
        ),
        "pm05b-facade-functions-at-most-50-lines",
    )
    contract.require(
        "#if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK)" in additions
        and "CONFIG_CACHEABLE_PSRAM_TASK_STACK" in config,
        "feature-off-pm-lifecycle-path-is-zero",
    )

    if contract.missing:
        print(
            f"PM-05B:SOURCE:FAIL checks={contract.checks} "
            f"missing={len(contract.missing)}",
            file=sys.stderr,
        )
        return 1
    print(f"PM-05B:SOURCE:PASS checks={contract.checks} missing=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
