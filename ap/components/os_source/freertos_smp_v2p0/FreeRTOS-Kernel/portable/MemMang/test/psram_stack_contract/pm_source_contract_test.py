#!/usr/bin/env python3

import re
import subprocess
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
MEMMANG_DIR = TEST_DIR.parent.parent
HEAP_SOURCE = MEMMANG_DIR / "heap_4.c"
PRIVATE_HEADER = MEMMANG_DIR / "psram_stack_allocator.h"
ALLOCATOR_TEST = TEST_DIR / "allocator_contract_test.c"
ACTUAL_TEST = TEST_DIR / "pm_actual_api_contract_test.c"
FEATURE_OFF_TEST = TEST_DIR / "pm_feature_off_actual_test.c"
MAKEFILE = TEST_DIR / "Makefile"
CACHE_INCLUDE_DIR = MEMMANG_DIR.parents[5] / "middleware/arch/cm33/include"


class Contract:
    def __init__(self):
        self.checks = 0
        self.missing = []

    def require(self, condition, name):
        self.checks += 1
        if not condition:
            self.missing.append(name)
            print(f"PM-05A:SOURCE:MISSING {name}", file=sys.stderr)


def extract_function(text, name):
    match = re.search(
        rf"(?m)^[ \t]*(?:static[ \t]+)?"
        rf"[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*]+"
        rf"{re.escape(name)}\s*\([^;{{}}]*\)[^;{{}}]*\{{",
        text,
    )
    if match is None:
        return ""
    start = match.start()
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


def has_order(text, patterns):
    position = 0
    for pattern in patterns:
        match = re.search(pattern, text[position:], re.DOTALL)
        if match is None:
            return False
        position += match.end()
    return True


def lock_windows(text):
    windows = []
    position = 0
    while True:
        start = text.find("HeapEnterCritical(", position)
        if start < 0:
            return windows
        end = text.find("HeapExitCritical(", start)
        if end < 0:
            return windows
        windows.append(text[start:end])
        position = end + 1


def functions_containing(text, token):
    results = []
    names = re.findall(
        r"(?m)^[ \t]*(?:static[ \t]+)?"
        r"[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*]+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)[^;{}]*\{",
        text,
    )
    for name in dict.fromkeys(names):
        body = extract_function(text, name)
        if token in body:
            results.append((name, body))
    return results


def rejects_provisional_before_metadata(body, guard_helpers, metadata_tokens):
    guard_positions = [body.find("s_psram_stack_pm_provisional_hold")]
    guard_positions.extend(
        body.find(f"{name}(") for name, unused_body in guard_helpers
    )
    guard_positions = [position for position in guard_positions if position >= 0]
    metadata_positions = [
        body.find(token) for token in metadata_tokens if body.find(token) >= 0
    ]
    if not guard_positions or not metadata_positions:
        return False
    guard = min(guard_positions)
    metadata = min(metadata_positions)
    return guard < metadata and "return" in body[guard:metadata]


def main():
    try:
        heap = HEAP_SOURCE.read_text(encoding="utf-8")
        header = PRIVATE_HEADER.read_text(encoding="utf-8")
        allocator_test = ALLOCATOR_TEST.read_text(encoding="utf-8")
        actual_test = ACTUAL_TEST.read_text(encoding="utf-8")
        feature_off_test = FEATURE_OFF_TEST.read_text(encoding="utf-8")
        makefile = MAKEFILE.read_text(encoding="utf-8")
    except OSError as error:
        print(f"PM-05A:SOURCE:HARNESS-FAIL {error}", file=sys.stderr)
        return 2

    feature_off = subprocess.run(
        [
            "cc",
            "-E",
            "-P",
            f"-I{TEST_DIR / 'stubs'}",
            f"-I{CACHE_INCLUDE_DIR}",
            f"-I{MEMMANG_DIR}",
            "-DCONFIG_MEM_DEBUG=1",
            "-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=0",
            str(HEAP_SOURCE),
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout

    contract = Contract()
    alloc_provider = extract_function(
        heap, "psram_stack_alloc_with_transition"
    )
    free_provider = extract_function(
        heap, "psram_stack_free_exact_with_transition"
    )
    hold_commit = extract_function(heap, "psram_stack_pm_hold_release_commit")
    rebuild_provider = extract_function(
        heap, "psram_heaps_rebuild_after_power_down_locked"
    )
    power_finish = extract_function(
        heap, "psram_stack_finish_power_guard_locked"
    )
    alloc_commit = extract_function(
        heap, "psram_stack_commit_allocation_locked"
    )
    free_commit = extract_function(
        heap, "psram_stack_commit_free_transition_locked"
    )
    generation_available = extract_function(
        heap, "psram_stack_generation_available_locked"
    )
    rebuild_commit = extract_function(
        heap, "psram_heaps_commit_rebuild_locked"
    )
    alloc = alloc_provider + power_finish + alloc_commit
    free = free_provider + free_commit
    rebuild = rebuild_provider + rebuild_commit
    power_failure_flow = power_finish + alloc_provider
    ordinary_alloc = extract_function(heap, "psram_malloc_cm")
    legacy_stack_alloc = extract_function(heap, "psram_stack_alloc")
    stack_allocator = alloc if alloc else legacy_stack_alloc
    power_prepare = extract_function(
        heap, "psram_stack_prepare_power_locked"
    )
    provisional_release = extract_function(
        heap, "psram_stack_release_provisional_locked"
    )
    forbidden = ("bk_pm_", "ipc_", "mailbox", "mb_ipc")

    contract.require(
        re.search(
            r"static\s+(?:volatile\s+)?uint32_t\s+s_psram_stack_live_count",
            heap,
        )
        is not None,
        "stack-live-counter-under-heap-owner",
    )
    contract.require(
        re.search(
            r"static\s+(?:volatile\s+)?uint64_t\s+"
            r"s_psram_stack_pm_hold_generation",
            heap,
        )
        is not None
        and re.search(
            r"static\s+(?:volatile\s+)?uint64_t\s+"
            r"s_psram_stack_pm_generation",
            heap,
        )
        is not None
        and "s_psram_stack_pm_hold = false" not in heap,
        "uint64-generation-owned-hold-replaces-boolean-hold",
    )
    contract.require(
        re.search(
            r"static\s+(?:volatile\s+)?(?:bool|uint32_t)\s+"
            r"s_psram_stack_pm_provisional_hold",
            heap,
        )
        is not None,
        "provisional-power-on-hold-has-heap-owned-state",
    )
    provisional_functions = functions_containing(
        heap, "s_psram_stack_pm_provisional_hold"
    )
    mutation_guard_helpers = [
        (name, body)
        for name, body in provisional_functions
        if re.search(r"(?:preflight|reject|inflight)", name)
    ]
    shared_prepare_names = [
        name
        for name, unused_body in provisional_functions
        if name in ordinary_alloc
        and name in stack_allocator
        and name not in (
            "psram_malloc_cm",
            "psram_stack_alloc_with_transition",
            "psram_heaps_rebuild_after_power_down_locked",
            "psram_stack_release_provisional_locked",
            "psram_stack_pm_hold_release_commit",
        )
    ]
    contract.require(
        bool(shared_prepare_names),
        "ordinary-and-stack-share-single-inflight-prepare-helper",
    )
    contract.require(
        bool(shared_prepare_names)
        and any(name in ordinary_alloc for name in shared_prepare_names),
        "ordinary-allocator-participates-in-single-inflight-guard",
    )
    contract.require(
        rejects_provisional_before_metadata(
            free,
            mutation_guard_helpers,
            (
                "cache_stack_",
                "psram_stack_validate",
                "psram_heaps_rebuild",
            ),
        ),
        "inflight-stale-free-rejected-before-rebuild-or-cache",
    )
    contract.require(
        rejects_provisional_before_metadata(
            hold_commit,
            mutation_guard_helpers,
            (
                "s_psram_stack_pm_hold_generation",
                "s_psram_used_count",
                "FIXED_ADDR_PSRAM_USDE_COUNT",
                "psram_heaps_rebuild",
                "cache_stack_",
            ),
        ),
        "inflight-stale-hold-ack-rejected-before-state-access",
    )
    contract.require(
        "PSRAM_STACK_PM_GENERATION_API_VERSION" in header
        and re.search(
            r"typedef\s+struct\s*\{[\s\S]*kind\s*;[\s\S]*"
            r"uint64_t\s+generation\s*;[\s\S]*\}\s*"
            r"psram_stack_pm_transition_t\s*;",
            header,
        )
        is not None,
        "private-header-transition-carries-generation",
    )
    contract.require(
        "psram_stack_alloc_with_transition(" in header,
        "private-header-allocation-transition-api",
    )
    contract.require(
        "psram_stack_free_exact_with_transition(" in header,
        "private-header-free-transition-api",
    )
    contract.require(
        re.search(
            r"psram_stack_pm_hold_release_commit\s*\(\s*uint64_t\s+"
            r"generation\s*\)",
            header,
        )
        is not None,
        "private-header-tokenized-ack-commit-api",
    )
    contract.require(
        '#include "../../heap_4.c"' in allocator_test
        and '#include "../../psram_stack_allocator.h"' in allocator_test,
        "host-test-directly-includes-actual-heap-and-private-header",
    )
    contract.require(
        all(
            token in actual_test
            for token in (
                "psram_stack_alloc_with_transition(",
                "psram_stack_free_exact_with_transition(",
                "psram_stack_pm_hold_release_commit(",
                "psram_heaps_rebuild_after_power_down_locked(",
                "pthread_create(",
                "peer-metadata-clean-before-all-free",
                "peer-stale-clean-refill-after-rebuild",
                "ordinary-first-dual-rebuild",
                "stack-first-power-on-outside-lock-recheck",
                "provisional-hold-before-power-on",
                "magic-toctou-recheck",
                "single-inflight-stack-leader",
                "single-inflight-ordinary-leader",
                "used-direct-ordinary-one-lock",
                "used-direct-stack-one-lock",
                "fragmented-oom-nonfatal",
                "corruption-is-fatal",
                "inflight-stale-free-preflight",
                "inflight-stale-ack-preflight",
            )
        )
        and ".dirty = true" not in actual_test,
        "actual-harness-directly-calls-production-pm-apis",
    )
    contract.require(
        "uint32_t generation" not in actual_test
        and re.search(
            r"actual_ack_commit\s*\(\s*uint64_t\s+generation\s*\)",
            actual_test,
        )
        is not None
        and all(
            re.search(
                rf"(?:const\s+)?uint64_t\s+{name}\b",
                actual_test,
            )
            is not None
            for name in (
                "old_generation",
                "new_generation",
                "stale_generation",
            )
        ),
        "actual-harness-preserves-full-uint64-generations",
    )
    contract.require(
        '#include "../../heap_4.c"' in feature_off_test
        and "CONFIG_CACHEABLE_PSRAM_TASK_STACK=0" in makefile
        and "PM_FEATURE_OFF_TARGET" in makefile
        and "PM-05A:FEATURE-OFF:PASS" in feature_off_test,
        "feature-off-actual-runtime-exercises-legacy-recovery",
    )
    contract.require(
        "-DCONFIG_MEM_DEBUG=1" in makefile
        and "$(ARM_CC)" in makefile
        and "$(HEAP_SOURCE)" in makefile,
        "host-and-arm-gates-build-actual-heap-with-mem-debug",
    )
    contract.require(
        "ARM_FEATURE_OFF_HEAP_TARGET" in makefile
        and "-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=0" in makefile
        and "PM_ACTUAL_TARGET" in makefile,
        "feature-off-arm-and-actual-api-gates-are-serial",
    )

    contract.require(
        bool(alloc)
        and has_order(
            alloc,
            [
                r"psram_stack_commit_allocation\s*\(",
                r"s_psram_stack_live_count\s*\+\+",
                r"s_psram_used_count\s*\+\+",
                r"FIXED_ADDR_PSRAM_USDE_COUNT\s*\+\+",
                r"(?:__DMB|portMEMORY_BARRIER)\s*\(",
            ],
        ),
        "committed-allocation-increments-all-counts-then-dmb",
    )
    contract.require(
        bool(alloc)
        and has_order(
            alloc,
            [
                r"transition\s*->\s*generation\s*=\s*0U?",
                r"s_psram_stack_live_count\s*==\s*1U?",
                r"transition\s*->\s*kind\s*=\s*"
                r"PSRAM_STACK_PM_TRANSITION_FIRST_LIVE",
            ],
        ),
        "first-live-transition-output",
    )
    contract.require(
        bool(free)
        and re.search(
            r"s_psram_stack_live_count\s*>\s*1U?[\s\S]*"
            r"s_psram_stack_live_count\s*--[\s\S]*"
            r"s_psram_used_count\s*--[\s\S]*"
            r"FIXED_ADDR_PSRAM_USDE_COUNT\s*--",
            free,
        )
        is not None,
        "non-final-free-decrements-all-counts",
    )
    contract.require(
        bool(free)
        and has_order(
            free,
            [
                r"psram_stack_commit_free\s*\(",
                r"s_psram_stack_live_count\s*--",
            ],
        ),
        "exact-free-commit-precedes-accounting",
    )
    contract.require(
        bool(free)
        and re.search(
            r"s_psram_stack_live_count\s*==\s*1U?[\s\S]*"
            r"s_psram_stack_pm_hold_generation[\s\S]*"
            r"transition\s*->\s*kind\s*=\s*"
            r"PSRAM_STACK_PM_TRANSITION_LAST_FREE[\s\S]*"
            r"transition\s*->\s*generation\s*=\s*"
            r"s_psram_stack_pm_hold_generation",
            free,
        )
        is not None,
        "last-free-retains-hold-and-outputs-generation",
    )
    contract.require(
        bool(free)
        and free_commit.count("s_psram_stack_pm_generation++") == 1
        and "s_psram_stack_pm_generation == 0" not in free_commit
        and has_order(
            free_commit,
            [
                r"s_psram_stack_pm_generation\s*\+\+",
                r"s_psram_stack_pm_hold_generation\s*=",
                r"transition\s*->\s*generation\s*=",
            ],
        ),
        "last-free-generation-is-uint64-monotonic-without-reuse",
    )
    contract.require(
        bool(generation_available)
        and "UINT64_MAX" in generation_available
        and all(
            token in generation_available
            for token in ("__DMB", "configPSRAM_STACK_FATAL_RESET")
        )
        and has_order(
            free_provider,
            [
                r"psram_stack_generation_available_locked\s*\(",
                r"psram_heaps_rebuild_after_power_down_locked\s*\(",
            ],
        ),
        "generation-exhaustion-fails-closed-before-mutation",
    )
    contract.require(
        bool(free)
        and re.search(
            r"(?:==\s*0U|>\s*s_psram_used_count|>\s*FIXED_ADDR_PSRAM_USDE_COUNT)",
            free,
        )
        is not None,
        "free-underflow-preflight",
    )
    contract.require(
        bool(hold_commit)
        and has_order(
            hold_commit,
            [
                r"generation\s*==\s*s_psram_stack_pm_hold_generation",
                r"s_psram_used_count\s*--",
                r"FIXED_ADDR_PSRAM_USDE_COUNT\s*--",
                r"s_psram_stack_pm_hold_generation\s*=\s*0U?",
                r"(?:__DMB|portMEMORY_BARRIER)\s*\(",
            ],
        ),
        "matching-token-ack-releases-old-hold",
    )
    contract.require(
        bool(hold_commit)
        and re.search(
            r"generation\s*==\s*0U?[\s\S]*|"
            r"s_psram_stack_pm_hold_generation\s*==\s*0U?",
            hold_commit,
        )
        is not None,
        "zero-stale-or-duplicate-ack-rejected",
    )
    contract.require(
        bool(hold_commit)
        and re.search(
            r"s_psram_stack_live_count\s*!=\s*0U?",
            hold_commit,
        )
        is None,
        "ack-allows-new-live-and-releases-only-old-hold",
    )
    contract.require(
        bool(hold_commit)
        and re.search(
            r"s_psram_used_count\s*<=\s*s_psram_stack_live_count",
            hold_commit,
        )
        is not None
        and re.search(
            r"FIXED_ADDR_PSRAM_USDE_COUNT\s*<=\s*"
            r"s_psram_stack_live_count",
            hold_commit,
        )
        is not None,
        "ack-requires-shared-and-fixed-count-above-live",
    )

    contract.require(
        bool(rebuild)
        and "FIXED_ADDR_PSRAM_POWER_DOWN" in rebuild
        and "PM_PSRAM_POWER_DOWN_MAGIC" in rebuild,
        "dual-rebuild-gated-by-existing-power-down-magic",
    )
    contract.require(
        bool(rebuild)
        and all(
            token in rebuild
            for token in (
                "s_psram_stack_live_count",
                "s_psram_stack_pm_hold_generation",
                "s_psram_used_count",
                "FIXED_ADDR_PSRAM_USDE_COUNT",
            )
        )
        and "configPSRAM_STACK_FATAL_RESET()" in rebuild
        and "BK_ASSERT" not in rebuild
        and "HeapEnterCritical(" not in rebuild,
        "magic-with-reference-uses-config-fatal-without-assert-or-extra-lock",
    )
    contract.require(
        bool(rebuild)
        and all(
            token in rebuild
            for token in (
                "psram_xStart",
                "psram_pxEnd",
                "psram_xFreeBytesRemaining",
                "psram_xMinimumEverFreeBytesRemaining",
                "xPsramUsed",
                "bk_psram_heap_init_flag_set",
                "s_psram_stack_start",
                "s_psram_stack_end",
                "s_psram_stack_free_bytes",
                "s_psram_stack_minimum_ever_free_bytes",
                "s_psram_used_count",
                "FIXED_ADDR_PSRAM_USDE_COUNT",
            )
        ),
        "one-helper-rebuilds-both-heaps-and-accounting",
    )
    contract.require(
        bool(rebuild)
        and has_order(
            rebuild,
            [
                r"psram_xStart",
                r"psram_pxEnd",
                r"s_psram_stack_start",
                r"s_psram_stack_end",
                r"FIXED_ADDR_PSRAM_POWER_DOWN\s*=\s*0",
            ],
        ),
        "magic-clears-after-both-heaps-rebuilt",
    )
    contract.require(
        bool(ordinary_alloc)
        and has_order(
            ordinary_alloc,
            [
                r"HeapEnterCritical\s*\(",
                r"psram_heaps_rebuild_after_power_down_locked\s*\(",
            ],
        ),
        "ordinary-allocator-cannot-consume-stack-rebuild-signal",
    )
    ordinary_lock = ordinary_alloc.find("HeapEnterCritical(")
    ordinary_prefix = (
        ordinary_alloc[:ordinary_lock] if ordinary_lock >= 0 else ordinary_alloc
    )
    contract.require(
        re.search(
            r"FIXED_ADDR_PSRAM_POWER_DOWN\s*=(?!=)",
            ordinary_prefix,
        )
        is None,
        "ordinary-allocator-does-not-clear-magic-before-locked-rebuild",
    )
    contract.require(
        bool(stack_allocator)
        and has_order(
            stack_allocator,
            [
                r"HeapEnterCritical\s*\(",
                r"psram_heaps_rebuild_after_power_down_locked\s*\(",
            ],
        ),
        "stack-allocator-uses-same-rebuild-helper",
    )
    contract.require(
        bool(stack_allocator)
        and has_order(
            stack_allocator,
            [
                r"HeapEnterCritical\s*\(",
                r"psram_stack_prepare_power_locked\s*\(",
                r"HeapExitCritical\s*\(",
                r"bk_pm_module_vote_psram_ctrl\s*\(",
                r"HeapEnterCritical\s*\(",
                r"psram_heaps_rebuild_after_power_down_locked\s*\(",
            ],
        )
        and has_order(
            power_prepare,
            [
                r"FIXED_ADDR_PSRAM_POWER_DOWN\s*"
                r"==\s*PM_PSRAM_POWER_DOWN_MAGIC",
                r"s_psram_used_count\s*\+\+",
                r"FIXED_ADDR_PSRAM_USDE_COUNT\s*\+\+",
                r"(?:__DMB|portMEMORY_BARRIER)\s*\(",
            ],
        ),
        "provisional-hold-dmb-power-outside-relock-recheck-order",
    )
    contract.require(
        bool(stack_allocator)
        and stack_allocator.count("HeapEnterCritical(") >= 2
        and stack_allocator.count("HeapExitCritical(") >= 2
        and re.search(
            r"s_psram_stack_pm_provisional_hold\s*=\s*(?:false|0U?)",
            stack_allocator,
        )
        is not None,
        "provisional-success-converts-without-double-accounting",
    )
    power_call = stack_allocator.find("bk_pm_module_vote_psram_ctrl(")
    power_window_start = stack_allocator.rfind("HeapExitCritical(", 0, power_call)
    power_window_end = stack_allocator.find("HeapEnterCritical(", power_call)
    power_window = (
        stack_allocator[power_window_start:power_window_end]
        if power_call >= 0
        and power_window_start >= 0
        and power_window_end >= 0
        else ""
    )
    contract.require(
        bool(power_window)
        and all(
            token not in power_window
            for token in (
                "s_psram_stack_",
                "s_psram_used_count",
                "FIXED_ADDR_",
                "psram_xStart",
                "psram_pxEnd",
                "cache_stack_",
            )
        ),
        "unlocked-power-window-reads-no-shared-psram-state",
    )
    first_stack_lock = stack_allocator.find("HeapEnterCritical(")
    prelock_stack = (
        stack_allocator[:first_stack_lock] if first_stack_lock >= 0 else ""
    )
    contract.require(
        bool(prelock_stack)
        and all(
            token not in prelock_stack
            for token in (
                "s_psram_stack_end",
                "s_psram_used_count",
                "FIXED_ADDR_PSRAM_POWER_DOWN",
                "FIXED_ADDR_PSRAM_USDE_COUNT",
            )
        ),
        "prelock-observation-reads-no-shared-psram-state",
    )
    contract.require(
        bool(power_failure_flow)
        and has_order(
            power_failure_flow,
            [
                r"(?:power_ret|power_result|ret)\s*=\s*"
                r"bk_pm_module_vote_psram_ctrl\s*\(",
                r"HeapEnterCritical\s*\(",
                r"(?:power_ret|power_result|ret)\s*!=\s*BK_OK",
                r"psram_stack_release_provisional_locked\s*\(",
                r"HeapExitCritical\s*\(",
                r"return\s+PSRAM_STACK_STATUS_",
            ],
        )
        and has_order(
            provisional_release,
            [
                r"s_psram_used_count\s*--",
                r"FIXED_ADDR_PSRAM_USDE_COUNT\s*--",
                r"s_psram_stack_pm_provisional_hold\s*=\s*(?:false|0U?)",
                r"(?:__DMB|portMEMORY_BARRIER)\s*\(",
            ],
        ),
        "power-fail-cleans-hold-before-return-without-metadata",
    )
    contract.require(
        bool(stack_allocator)
        and re.search(
            r"prepare_result\s*==\s*PSRAM_STACK_STATUS_NO_MEMORY[\s\S]*"
            r"psram_stack_release_provisional_locked\s*\(",
            stack_allocator,
        )
        is not None,
        "ordinary-oom-cleans-provisional-hold",
    )
    contract.require(
        bool(stack_allocator)
        and "configPSRAM_STACK_FATAL_RESET()" in stack_allocator
        and re.search(
            r"(?:rebuild|cache|invariant)[\s\S]*"
            r"configPSRAM_STACK_FATAL_RESET\s*\(\s*\)",
            stack_allocator,
            re.IGNORECASE,
        )
        is not None,
        "rebuild-or-cache-invariant-failure-is-fatal",
    )
    new_provisional_functions = [
        body
        for name, body in functions_containing(
            heap, "s_psram_stack_pm_provisional_hold"
        )
        if name
        not in (
            "psram_stack_alloc_with_transition",
            "psram_stack_free_exact_with_transition",
            "psram_stack_pm_hold_release_commit",
            "psram_heaps_rebuild_after_power_down_locked",
            "psram_malloc_cm",
        )
    ]
    contract.require(
        all(len(body.splitlines()) <= 50 for body in new_provisional_functions),
        "new-provisional-helper-functions-at-most-50-lines",
    )
    for function_name, function_body in (
        ("psram-stack-alloc", alloc_provider),
        ("psram-stack-free", free_provider),
        ("psram-hold-commit", hold_commit),
        ("psram-dual-rebuild", rebuild_provider),
    ):
        contract.require(
            bool(function_body) and len(function_body.splitlines()) <= 50,
            f"{function_name}-function-at-most-50-lines",
        )
    all_windows = (
        lock_windows(ordinary_alloc)
        + lock_windows(alloc_provider)
        + lock_windows(free_provider)
        + lock_windows(hold_commit)
    )
    contract.require(
        bool(all_windows)
        and all(
            token not in window
            for window in all_windows
            for token in forbidden
        ),
        "no-pm-or-mailbox-call-inside-heap-lock",
    )

    feature_off_alloc = extract_function(feature_off, "psram_malloc_cm")
    contract.require(
        "psram_stack_alloc_with_transition" not in feature_off
        and "psram_heaps_rebuild_after_power_down_locked" not in feature_off
        and bool(feature_off_alloc)
        and has_order(
            feature_off_alloc,
            [
                r"vPortEnterCritical\s*\(",
                r"psram_stack_prepare_power_locked\s*\(",
                r"vPortExitCritical\s*\(",
                r"bk_pm_module_vote_psram_ctrl\s*\(",
                r"vPortEnterCritical\s*\(",
                r"psram_heap_recover_power_down_legacy\s*\(",
                r"psram_malloc_without_lock\s*\(",
            ],
        ),
        "feature-off-powerdown-recovery-uses-shared-safe-guard",
    )

    if contract.missing:
        print(
            f"PM-05A:SOURCE:FAIL checks={contract.checks} "
            f"missing={len(contract.missing)}",
            file=sys.stderr,
        )
        return 1
    print(f"PM-05A:SOURCE:PASS checks={contract.checks} missing=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
