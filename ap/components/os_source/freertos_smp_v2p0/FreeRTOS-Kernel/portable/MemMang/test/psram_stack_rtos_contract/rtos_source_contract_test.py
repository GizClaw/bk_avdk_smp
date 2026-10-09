#!/usr/bin/env python3

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

FEATURE = "CONFIG_CACHEABLE_PSRAM_TASK_STACK"
TEST_DIR = Path(__file__).resolve().parent
KERNEL_DIR = TEST_DIR.parents[3]
SDK_DIR = next(parent.parent for parent in TEST_DIR.parents if parent.name == "ap")
BASELINE_FILE = TEST_DIR / "feature_off_baseline.json"

FILES = {
    "additions": KERNEL_DIR / "include/additions/freertos_tasks_c_additions.h",
    "tasks": KERNEL_DIR / "tasks.c",
    "port": KERNEL_DIR / "portable/GCC/ARM_CM33_NTZ/non_secure/port.c",
    "freertos_header": KERNEL_DIR / "include/freertos/FreeRTOS.h",
    "allocator_header": KERNEL_DIR / "portable/MemMang/psram_stack_allocator.h",
    "heap": KERNEL_DIR / "portable/MemMang/heap_4.c",
    "freertos_config": SDK_DIR / "ap/components/bk_rtos/freertos/FreeRTOSConfig.h",
    "kconfig": SDK_DIR / "ap/components/bk_rtos/Kconfig",
    "rtos_pub": SDK_DIR / "ap/components/bk_rtos/freertos/v10/rtos_pub_smp.c",
    "mpu": SDK_DIR / "ap/middleware/soc/bk7258_ap/mpu_cfg.c",
}

def read_sources(paths):
    return {name: path.read_text(encoding="utf-8") for name, path in paths.items()}


def extract_braced(text, start):
    brace = text.find("{", start)
    if brace < 0:
        raise ValueError("opening brace not found")
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    raise ValueError("closing brace not found")


def extract_function(text, name):
    pattern = re.compile(
        rf"\b{re.escape(name)}\s*\([^;{{}}]*\)[^;{{}}]*\{{",
        re.DOTALL,
    )
    match = pattern.search(text)
    if match is None:
        raise ValueError(f"function {name} not found")
    return extract_braced(text, match.start())


def extract_tcb(text):
    start = text.index("typedef struct tskTaskControlBlock")
    end = text.index("} tskTCB;", start) + len("} tskTCB;")
    return text[start:end]


def extract_static_task(text):
    start = text.index("typedef struct xSTATIC_TCB")
    end = text.index("} StaticTask_t;", start) + len("} StaticTask_t;")
    return text[start:end]


def extract_kconfig_block(text, symbol):
    match = re.search(rf"^\s*config\s+{re.escape(symbol)}\s*$", text, re.MULTILINE)
    if match is None:
        raise ValueError(f"Kconfig symbol {symbol} not found")
    next_match = re.search(r"^\s*config\s+\w+\s*$", text[match.end() :], re.MULTILINE)
    end = match.end() + next_match.start() if next_match is not None else len(text)
    return text[match.start() : end]


def extract_initializer(text, name):
    match = re.search(rf"\b{re.escape(name)}\s*\[\s*\]\s*=\s*\{{", text)
    if match is None:
        raise ValueError(f"initializer {name} not found")
    block = extract_braced(text, match.start())
    return block


def strip_feature_blocks(text, feature_enabled=False):
    output = []
    stack = []
    active = True

    for line in text.splitlines():
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive is None:
            if active:
                output.append(line)
            continue

        kind = directive.group(1)
        expression = directive.group(2)
        if kind in ("if", "ifdef", "ifndef"):
            feature_condition = FEATURE in expression
            if feature_condition:
                if kind == "ifndef":
                    condition = not feature_enabled
                else:
                    condition = feature_enabled
                stack.append((active, True, condition))
                active = active and condition
            else:
                stack.append((active, False, True))
                if active:
                    output.append(line)
        elif kind in ("else", "elif"):
            if not stack:
                raise ValueError("unbalanced preprocessor branch")
            parent_active, feature_condition, condition = stack[-1]
            if feature_condition:
                if kind == "else":
                    condition = not condition
                else:
                    condition = feature_enabled
                stack[-1] = (parent_active, feature_condition, condition)
                active = parent_active and condition
            elif active:
                output.append(line)
        else:
            if not stack:
                raise ValueError("unbalanced preprocessor end")
            parent_active, feature_condition, _ = stack.pop()
            active = parent_active
            if not feature_condition and active:
                output.append(line)

    if stack:
        raise ValueError("unterminated preprocessor branch")
    return "\n".join(output)


def normalize(text):
    return re.sub(r"\s+", " ", text).strip()


def digest(text):
    return hashlib.sha256(normalize(text).encode("utf-8")).hexdigest()


def baseline_sections(sources):
    stripped = {name: strip_feature_blocks(text) for name, text in sources.items()}
    return {
        "additions:xTaskCreate_ex": extract_function(
            stripped["additions"], "xTaskCreate_ex"
        ),
        "additions:xTaskCreatePinnedToCore": extract_function(
            stripped["additions"], "xTaskCreatePinnedToCore"
        ),
        "additions:xTaskCreateInPsram": extract_function(
            stripped["additions"], "xTaskCreateInPsram"
        ),
        "additions:xTaskCreateStaticPinnedToCore": extract_function(
            stripped["additions"], "xTaskCreateStaticPinnedToCore"
        ),
        "tasks:TCB": extract_tcb(stripped["tasks"]),
        "freertos_header:StaticTask": extract_static_task(
            stripped["freertos_header"]
        ),
        "tasks:vTaskSwitchContext": extract_function(
            stripped["tasks"], "vTaskSwitchContext"
        ),
        "tasks:prvDeleteTCB": extract_function(stripped["tasks"], "prvDeleteTCB"),
        "port:vPortSVCHandler_C": extract_function(
            stripped["port"], "vPortSVCHandler_C"
        ),
        "rtos_pub:rtos_create_thread": extract_function(
            stripped["rtos_pub"], "rtos_create_thread"
        ),
        "rtos_pub:rtos_smp_create_thread": extract_function(
            stripped["rtos_pub"], "rtos_smp_create_thread"
        ),
        "rtos_pub:rtos_create_thread_static": extract_function(
            stripped["rtos_pub"], "rtos_create_thread_static"
        ),
        "mpu:mpu_regions": extract_initializer(stripped["mpu"], "mpu_regions"),
    }


def feature_token_is_guarded(text):
    feature_depth = 0
    generic_depth = 0
    failures = []

    for number, line in enumerate(text.splitlines(), start=1):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|endif)\b(.*)", line)
        if directive is not None:
            kind = directive.group(1)
            expression = directive.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                if FEATURE in expression:
                    feature_depth += 1
                elif feature_depth > 0:
                    generic_depth += 1
            elif feature_depth > 0 and generic_depth > 0:
                generic_depth -= 1
            elif feature_depth > 0:
                feature_depth -= 1
            continue

        if "psram_stack_" in line and "#include" not in line and feature_depth == 0:
            failures.append(number)
    return failures


def has_order(text, patterns):
    cursor = 0
    for pattern in patterns:
        match = re.search(pattern, text[cursor:], re.DOTALL)
        if match is None:
            return False
        cursor += match.end()
    return True


def fail_stop_positions(text):
    return [
        match.start()
        for match in re.finditer(
            r"\b[A-Za-z_][A-Za-z0-9_]*(?:fail_stop|fatal)"
            r"[A-Za-z0-9_]*\s*\(",
            text,
            re.IGNORECASE,
        )
    ]


def failure_routes_to_fail_stop(text, call_name, before_token=None):
    call_position = text.find(f"{call_name}(")
    if call_position < 0 or f"( void ) {call_name}(" in text:
        return False
    end = text.find(before_token, call_position) if before_token is not None else len(text)
    if end < 0:
        end = len(text)
    return any(call_position < position < end for position in fail_stop_positions(text))


class Contract:
    def __init__(self):
        self.checks = 0
        self.missing = []

    def require(self, condition, name):
        self.checks += 1
        if not condition:
            self.missing.append(name)


def check_contract(sources):
    contract = Contract()
    additions = sources["additions"]
    tasks = sources["tasks"]
    port = sources["port"]
    freertos_header = sources["freertos_header"]
    allocator_header = sources["allocator_header"]
    heap = sources["heap"]
    freertos_config = sources["freertos_config"]
    kconfig = sources["kconfig"]
    rtos_pub = sources["rtos_pub"]
    mpu = sources["mpu"]

    for name in (
        "additions",
        "tasks",
        "port",
        "freertos_config",
        "rtos_pub",
        "mpu",
    ):
        contract.require(FEATURE in sources[name], f"feature-gate-{name}")
    kconfig_block = extract_kconfig_block(kconfig, "CACHEABLE_PSRAM_TASK_STACK")
    contract.require(
        re.search(r"^\s*depends\s+on\s+MPU\s*$", kconfig_block, re.MULTILINE)
        is not None,
        "kconfig-depends-on-mpu",
    )

    contract.require(
        "psram_stack_allocator.h" in additions,
        "creation-allocator-private-header",
    )
    contract.require(
        "psram_stack_alloc(" in additions
        or "psram_stack_alloc_with_transition(" in additions,
        "creation-stack-allocator",
    )
    contract.require(
        re.search(r"psram_malloc\s*\(\s*sizeof\s*\(\s*TCB_t\s*\)", additions)
        is not None,
        "creation-tcb-ordinary-psram",
    )

    tcb = extract_tcb(tasks)
    static_task = extract_static_task(freertos_header)
    contract.require(
        "xPsramStackOwnerSpan" not in tcb
        and "psram_stack_maintenance_span_t" not in tcb,
        "tcb-forbids-owner-span-field",
    )
    contract.require(
        "pxDummy24" not in static_task
        and "xDummy25" not in static_task
        and FEATURE not in static_task,
        "static-task-forbids-owner-span-padding",
    )
    contract.require(
        normalize(strip_feature_blocks(tcb, False))
        == normalize(strip_feature_blocks(tcb, True)),
        "tcb-feature-on-off-layout-identical",
    )
    contract.require(
        normalize(strip_feature_blocks(static_task, False))
        == normalize(strip_feature_blocks(static_task, True)),
        "static-task-feature-on-off-layout-identical",
    )
    layout_assertions = additions + tasks
    contract.require(
        re.search(
            r"(?:_Static_assert|static_assert)\s*\([^;]*"
            r"sizeof\s*\(\s*TCB_t\s*\)[^;]*"
            r"sizeof\s*\(\s*StaticTask_t\s*\)",
            layout_assertions,
            re.DOTALL,
        )
        is not None,
        "compile-time-tcb-static-size-equality",
    )
    contract.require(
        re.search(
            r"(?:_Static_assert|static_assert)\s*\([^;]*"
            r"(?:_Alignof|alignof)\s*\(\s*TCB_t\s*\)[^;]*"
            r"(?:_Alignof|alignof)\s*\(\s*StaticTask_t\s*\)",
            layout_assertions,
            re.DOTALL,
        )
        is not None,
        "compile-time-tcb-static-alignment-equality",
    )
    contract.require(
        not feature_token_is_guarded(additions),
        "feature-off-guard-additions",
    )
    contract.require(
        not feature_token_is_guarded(tasks),
        "feature-off-guard-tasks",
    )

    fatal_sources = additions + tasks
    contract.require(
        re.search(
            r"(?:_Noreturn|noreturn)[^;{]*(?:fail_stop|fatal)"
            r"|(?:fail_stop|fatal)[^;{]*(?:_Noreturn|noreturn)",
            fatal_sources,
            re.IGNORECASE | re.DOTALL,
        )
        is not None,
        "noreturn-kernel-fail-stop-seam",
    )
    fatal_function = extract_function(tasks, "prvPsramStackKernelFatal")
    platform_hook_name = "rtos_psram_stack_fatal_reset"
    try:
        platform_fatal_hook = extract_function(rtos_pub, platform_hook_name)
    except ValueError:
        platform_fatal_hook = ""
    forbidden_fatal_tokens = (
        "bk_reboot(",
        "bk_reboot_ex(",
        "bk_reboot_callback",
        "ipc_",
        "mb_ipc",
        "mailbox",
    )
    contract.require(
        re.search(
            r"#\s*define\s+configPSRAM_STACK_FATAL_RESET\s*\(\s*\)"
            r"[\s\S]*?rtos_psram_stack_fatal_reset\s*\(\s*\)",
            freertos_config,
        )
        is not None,
        "freertos-config-defines-direct-fatal-reset-hook",
    )
    contract.require(
        bool(platform_fatal_hook),
        "rtos-platform-defines-direct-fatal-reset-hook",
    )
    contract.require(
        "configPSRAM_STACK_FATAL_RESET(" in fatal_function,
        "fatal-seam-uses-config-platform-reset-hook",
    )
    contract.require(
        has_order(
            fatal_function,
            [
                r"ulPsramStackFatalReason\s*=",
                r"xPsramStackFatalCore\s*=",
                r"(?:__DMB|portMEMORY_BARRIER)\s*\(",
                r"configPSRAM_STACK_FATAL_RESET\s*\(\s*\)",
            ],
        ),
        "fatal-seam-records-reason-core-dmb-before-hook",
    )
    contract.require(
        has_order(
            fatal_function,
            [
                r"configPSRAM_STACK_FATAL_RESET\s*\(\s*\)",
                r"(?:portDISABLE_INTERRUPTS|__disable_irq)\s*\(",
                r"(?:for\s*\(\s*;\s*;\s*\)|while\s*\(\s*(?:1|true)\s*\))",
            ],
        ),
        "fatal-seam-local-halt-only-after-wdt-hook-return",
    )
    contract.require(
        all(token not in fatal_function for token in forbidden_fatal_tokens),
        "fatal-seam-forbids-general-reboot-callback-ipc",
    )
    contract.require(
        bool(platform_fatal_hook)
        and has_order(
            platform_fatal_hook,
            [
                r"bk_misc_set_reset_reason\s*\(\s*RESET_SOURCE_CRASH_ASSERT\s*\)",
                r"bk_wdt_force_reboot\s*\(\s*\)",
            ],
        ),
        "rtos-platform-hook-sets-reason-then-forces-wdt",
    )
    contract.require(
        bool(platform_fatal_hook)
        and all(
            token not in platform_fatal_hook for token in forbidden_fatal_tokens
        ),
        "rtos-platform-hook-forbids-general-reboot-callback-ipc",
    )

    explicit_create = extract_function(additions, "xTaskCreateInPsram")
    allocate = extract_function(additions, "prvAllocateCacheablePsramTask")
    ordinary_create = extract_function(additions, "xTaskCreatePinnedToCore")
    static_create = extract_function(additions, "xTaskCreateStaticPinnedToCore")
    allocate_position = explicit_create.find("prvAllocateCacheablePsramTask(")
    isr_positions = [
        explicit_create.find(token)
        for token in (
            "portCHECK_IF_IN_ISR(",
            "xPortInIsrContext(",
            "xPortIsInsideInterrupt(",
            "platform_is_in_interrupt_context(",
        )
        if token in explicit_create
    ]
    irq_positions = [
        explicit_create.find(token)
        for token in ("platform_local_irq_disabled(", "__get_PRIMASK(")
        if token in explicit_create
    ]
    contract.require(
        isr_positions
        and min(isr_positions) < allocate_position,
        "explicit-create-rejects-isr-before-allocation",
    )
    contract.require(
        irq_positions
        and min(irq_positions) < allocate_position,
        "explicit-create-rejects-irq-disabled-before-allocation",
    )

    contract.require(
        has_order(
            explicit_create,
            [
                r"(?:task|prv)ENTER_CRITICAL[^(]*\(\s*&xKernelLock\s*\)",
                r"prvInitialiseNewTask\s*\(",
                r"psram_stack_cache_release\s*\(",
                r"(?:task|prv)EXIT_CRITICAL[^(]*\(\s*&xKernelLock\s*\)",
                r"configPSRAM_STACK_PM_VOTE_ACTIVE\s*\(",
                r"prvAddNewTaskToReadyList\s*\(",
            ],
        ),
        "creation-init-release-active-before-publication",
    )
    contract.require(
        not any(
            token in explicit_create + tasks
            for token in (
                "prvPsramStackLifecycleLock",
                "LIFECYCLE_OWNER",
                "LIFECYCLE_TRY_LOCK",
                "LIFECYCLE_UNLOCK",
            )
        ),
        "rtos-has-no-task-owned-lifecycle-mutex-participant",
    )
    contract.require(FEATURE in explicit_create, "explicit-create-feature-route")
    contract.require(
        "psram_stack_" not in ordinary_create,
        "ordinary-create-legacy-route",
    )
    contract.require(
        "psram_stack_" not in static_create,
        "static-create-legacy-route",
    )
    contract.require(
        "psram_stack_alloc_with_transition(" in allocate
        and "owner_span" not in allocate,
        "allocation-does-not-export-owner-span",
    )
    contract.require(
        has_order(
            explicit_create,
            [
                r"psram_stack_cache_release\s*\(",
                r"(?:task|prv)EXIT_CRITICAL[^(]*\(\s*&xKernelLock\s*\)",
                r"prvFreeUnpublishedCacheablePsramTask\s*\(",
                r"prvAddNewTaskToReadyList\s*\(",
            ],
        ),
        "creation-release-failure-rolls-back-before-publication",
    )
    rollback = extract_function(additions, "prvFreeUnpublishedCacheablePsramTask")
    contract.require(
        failure_routes_to_fail_stop(
            rollback, "psram_stack_free_exact_with_transition"
        ),
        "creation-rollback-failure-fail-stop",
    )
    contract.require(
        "vPortFree(" in rollback,
        "creation-successful-rollback-frees-tcb",
    )

    switch = extract_function(tasks, "vTaskSwitchContext")
    contract.require(
        has_order(
            switch,
            [
                r"(?:task|prv)ENTER_CRITICAL[^(]*\(\s*&xKernelLock\s*\)",
                r"psram_stack_cache_release\s*\(",
                r"taskSELECT_HIGHEST_PRIORITY_TASK\s*\(",
                r"psram_stack_cache_acquire\s*\(",
                r"(?:task|prv)EXIT_CRITICAL[^(]*\(\s*&xKernelLock\s*\)",
            ],
        ),
        "switch-release-select-acquire-order",
    )
    contract.require(
        failure_routes_to_fail_stop(
            switch,
            "psram_stack_cache_release",
            "taskSELECT_HIGHEST_PRIORITY_TASK",
        ),
        "switch-out-release-failure-fail-stop-before-select",
    )
    switch_after_select = switch[switch.find("taskSELECT_HIGHEST_PRIORITY_TASK") :]
    contract.require(
        failure_routes_to_fail_stop(
            switch_after_select,
            "psram_stack_cache_acquire",
            "traceTASK_SWITCHED_IN",
        ),
        "switch-in-acquire-failure-fail-stop-before-restore",
    )
    contract.require(
        "psram_stack_alloc(" not in switch
        and "psram_stack_free_exact(" not in switch,
        "switch-never-takes-heap-lock",
    )
    contract.require(
        re.search(
            r"psram_stack_cache_release\s*\([^;]*pxStack[^;]*ulStackSize",
            switch,
            re.DOTALL,
        )
        is not None
        and re.search(
            r"psram_stack_cache_acquire\s*\([^;]*pxStack[^;]*ulStackSize",
            switch,
            re.DOTALL,
        )
        is not None,
        "switch-reconstructs-span-from-existing-tcb-fields",
    )

    svc = extract_function(port, "vPortSVCHandler_C")
    acquire_tokens = (
        "psram_stack_cache_acquire(",
        "vTaskCacheAcquireFirstStack(",
    )
    acquire_positions = [svc.find(token) for token in acquire_tokens if token in svc]
    acquire_position = min(acquire_positions) if acquire_positions else -1
    restore_position = svc.find("vRestoreContextOfFirstTask(")
    contract.require(
        acquire_position >= 0
        and restore_position >= 0
        and acquire_position < restore_position,
        "ntz-first-task-acquire-before-restore",
    )
    contract.require(
        "portGET_CORE_ID(" in svc
        or (
            "vTaskCacheAcquireFirstStack(" in svc
            and re.search(
                r"vTaskCacheAcquireFirstStack\s*\([^)]*\).*?portGET_CORE_ID\s*\(",
                tasks,
                re.DOTALL,
            )
            is not None
        ),
        "ntz-first-task-both-ap-cores",
    )
    first_acquire = extract_function(tasks, "vTaskCacheAcquireFirstStack")
    contract.require(
        failure_routes_to_fail_stop(first_acquire, "psram_stack_cache_acquire"),
        "first-task-acquire-failure-fail-stop-before-restore",
    )
    contract.require(
        "pxStack" in first_acquire and "ulStackSize" in first_acquire,
        "first-task-reconstructs-span-from-existing-tcb-fields",
    )

    delete_tcb = extract_function(tasks, "prvDeleteTCB")
    exact_free_position = delete_tcb.find(
        "psram_stack_free_exact_with_transition("
    )
    tcb_free_positions = [
        position
        for position in (
            delete_tcb.find("vPortFree( pxTCB )"),
            delete_tcb.find("vPortFree(pxTCB)"),
        )
        if position >= 0
    ]
    tcb_free_position = min(tcb_free_positions) if tcb_free_positions else -1
    contract.require(
        exact_free_position >= 0
        and tcb_free_position >= 0
        and exact_free_position < tcb_free_position,
        "delete-exact-free-before-tcb-free",
    )
    contract.require(
        failure_routes_to_fail_stop(
            delete_tcb, "psram_stack_free_exact_with_transition"
        ),
        "delete-exact-free-guard-failure-observable-fail-stop",
    )
    contract.require(
        re.search(
            r"psram_stack_free_exact_with_transition\s*\("
            r"[^;]*pxStack[^;]*ulStackSize",
            delete_tcb,
            re.DOTALL,
        )
        is not None,
        "delete-reconstructs-span-from-existing-tcb-fields",
    )
    contract.require("xKernelLock" not in delete_tcb, "delete-no-kernel-heap-nesting")

    delete_api = extract_function(tasks, "vTaskDelete")
    contract.require(
        has_order(
            delete_api,
            [
                r"(?:task|prv)EXIT_CRITICAL[^(]*\(\s*&xKernelLock\s*\)",
                r"prvDeleteTCB\s*\(",
            ],
        ),
        "delete-remove-unlock-before-free",
    )
    idle_delete = extract_function(tasks, "prvCheckTasksWaitingTermination")
    contract.require(
        has_order(
            idle_delete,
            [
                r"(?:task|prv)EXIT_CRITICAL[^(]*\(\s*&xKernelLock\s*\)",
                r"prvDeleteTCB\s*\(",
            ],
        ),
        "idle-delete-unlock-before-free",
    )

    live_span = extract_function(heap, "psram_stack_live_span_valid")
    contract.require(
        re.search(
            r"cache_stack_acquire_aligned\s*\(\s*"
            r"(?:maintenance_span->addr|payload)\s*,\s*"
            r"(?:maintenance_span->size|ownership_size)\s*\)",
            live_span,
            re.DOTALL,
        )
        is not None,
        "exact-free-acquires-complete-owner-span",
    )
    contract.require(
        "pxStack" in allocator_header
        and "ulStackSize" in allocator_header
        and "maintenance_span_t *maintenance_span" not in allocator_header,
        "allocator-facade-reconstructs-from-stack-and-size",
    )

    ordinary_wrapper = extract_function(rtos_pub, "rtos_create_thread")
    smp_wrapper = extract_function(rtos_pub, "rtos_smp_create_thread")
    static_wrapper = extract_function(rtos_pub, "rtos_create_thread_static")
    ordinary_feature_on = strip_feature_blocks(ordinary_wrapper, feature_enabled=True)
    smp_feature_on = strip_feature_blocks(smp_wrapper, feature_enabled=True)
    contract.require(
        FEATURE in ordinary_wrapper
        and "rtos_create_psram_thread" not in ordinary_feature_on
        and re.search(r"(ordinary|legacy|noncache)", ordinary_feature_on, re.IGNORECASE)
        is not None,
        "ordinary-wrapper-noncache-legacy-route",
    )
    contract.require(
        FEATURE in smp_wrapper
        and "rtos_create_psram_thread" not in smp_feature_on
        and re.search(r"(ordinary|legacy|noncache)", smp_feature_on, re.IGNORECASE)
        is not None,
        "smp-wrapper-noncache-legacy-route",
    )
    contract.require(
        "xTaskCreateStaticPinnedToCore" in static_wrapper
        and "xTaskCreateInPsram" not in static_wrapper,
        "static-wrapper-legacy-route",
    )
    contract.require(
        rtos_pub.count("xTaskCreateInPsram(") >= 3,
        "explicit-platform-wrappers-use-cacheable-route",
    )

    contract.require(
        "flush_dcache(" not in additions + tasks + port
        and "invalidate_dcache(" not in additions + tasks + port,
        "rtos-uses-allocator-facade-only",
    )
    contract.require(
        "configFLUSH_DCACHE_ON_TASK_SWITCH_OUT" not in additions + tasks + port,
        "no-source-branch-legacy-switch-gate",
    )
    contract.require(
        re.search(
            r"(?:_Static_assert|static_assert)\s*\([^;]*"
            r"(?:ARRAY_SIZE|sizeof\s*\(\s*mpu_regions\s*\))[^;]*16",
            mpu,
            re.DOTALL,
        )
        is not None,
        "mpu-region-count-static-assert",
    )

    baseline = json.loads(BASELINE_FILE.read_text(encoding="utf-8"))
    current_sections = baseline_sections(sources)
    for name, section in current_sections.items():
        contract.require(
            baseline.get(name) == digest(section),
            f"feature-off-zero-drift-{name}",
        )

    return contract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--emit-baseline", action="store_true")
    args = parser.parse_args()

    try:
        sources = read_sources(FILES)
        if args.emit_baseline:
            hashes = {
                name: digest(section)
                for name, section in baseline_sections(sources).items()
            }
            print(json.dumps(hashes, indent=2, sort_keys=True))
            return 0

        contract = check_contract(sources)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"RTOS-04:SOURCE:HARNESS-FAIL {error}", file=sys.stderr)
        return 2

    for missing in contract.missing:
        print(f"RTOS-04:SOURCE:MISSING {missing}")

    if contract.missing:
        print(
            f"RTOS-04:SOURCE:FAIL checks={contract.checks} "
            f"missing={len(contract.missing)}"
        )
        return 1

    print(f"RTOS-04:SOURCE:PASS checks={contract.checks} missing=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
