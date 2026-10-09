#!/usr/bin/env python3

import re
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
SDK_ROOT = TEST_DIR.parents[8]
AP_SOURCE = SDK_ROOT / "ap/middleware/driver/pwr_clk/pwr_clk.c"
CP_HANDLER_SOURCE = SDK_ROOT / "cp/middleware/driver/pwr_clk/low_pwr_core.c"
CP_PROVIDER_SOURCE = SDK_ROOT / "cp/middleware/driver/pwr_clk/pwr_clk.c"


def extract_function(text, name):
    match = re.search(
        rf"(?m)^[ \t]*(?:static[ \t]+)?"
        rf"[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*]+"
        rf"{re.escape(name)}\s*\([^;{{}}]*\)[^;{{}}]*\{{",
        text,
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


def expected_ap_behavior(response_seen, cp_ret, power_on):
    success = response_seen and cp_ret == 0
    return (0 if success else -1, success and power_on)


class Contract:
    def __init__(self):
        self.checks = 0
        self.missing = []

    def require(self, condition, name):
        self.checks += 1
        if not condition:
            self.missing.append(name)
            print(f"PM-05A:PROXY:MISSING {name}", file=sys.stderr)


def main():
    try:
        ap = AP_SOURCE.read_text(encoding="utf-8")
        cp_handler = CP_HANDLER_SOURCE.read_text(encoding="utf-8")
        cp_provider = CP_PROVIDER_SOURCE.read_text(encoding="utf-8")
    except OSError as error:
        print(f"PM-05A:PROXY:HARNESS-FAIL {error}", file=sys.stderr)
        return 2

    contract = Contract()
    ap_vote = extract_function(ap, "bk_pm_module_vote_psram_ctrl")
    ap_response = extract_case(ap, "PM_CTRL_PSRAM_POWER_CMD")
    cp_response = extract_case(cp_handler, "LOW_PWR_CORE_PSRAM_POWER")
    cp_power = extract_function(cp_provider, "pm_psram_power_ctrl")

    contract.require(
        re.search(
            r"pm_cp1_mailbox_send_data\s*\(\s*PM_CTRL_PSRAM_POWER_CMD"
            r"\s*,\s*module\s*,\s*power_state\s*,\s*0\s*\)",
            ap_vote,
        )
        is not None,
        "ap-request-command-and-fields-remain-compatible",
    )
    contract.require(
        has_order(
            cp_response,
            [
                r"ret\s*=\s*bk_pm_module_vote_psram_ctrl\s*\("
                r"\s*msg\.param1\s*,\s*msg\.param2\s*\)",
                r"bk_pm_cp0_response_cp1\s*\(\s*PM_CTRL_PSRAM_POWER_CMD"
                r"\s*,\s*msg\.param2\s*,\s*ret\s*,\s*0\s*\)",
            ],
        ),
        "cp-response-param1-requested-state-param2-real-ret",
    )
    contract.require(
        "cmd_buf->param1" in ap_response
        and re.search(
            r"s_pm_psram_power_(?:ret|result)\s*=\s*cmd_buf->param2",
            ap_response,
        )
        is not None,
        "ap-response-captures-param2-ret",
    )
    contract.require(
        re.search(
            r"if\s*\(\s*!\s*bk_pm_cp1_psram_power_state_get\s*\(\s*\)"
            r"\s*\)\s*\{[\s\S]*return\s+BK_FAIL\s*;",
            ap_vote,
        )
        is not None,
        "ap-timeout-returns-bk-fail",
    )
    contract.require(
        re.search(
            r"s_pm_psram_power_(?:ret|result)\s*!=\s*BK_OK"
            r"[\s\S]*return\s+BK_FAIL\s*;",
            ap_vote,
        )
        is not None,
        "ap-nack-param2-returns-bk-fail",
    )
    contract.require(
        has_order(
            ap_vote,
            [
                r"return\s+BK_FAIL\s*;",
                r"s_pm_psram_power_(?:ret|result)\s*!=\s*BK_OK",
                r"return\s+BK_FAIL\s*;",
                r"bk_pm_ap_psram_power_state_handle_callback\s*\(",
                r"return\s+BK_OK\s*;",
            ],
        ),
        "ap-callback-runs-only-after-success-ack",
    )
    contract.require(
        bool(ap_vote) and len(ap_vote.splitlines()) <= 50,
        "matched-provider-function-at-most-50-lines",
    )
    contract.require(
        all(
            token in ap
            for token in (
                "PM_PSRAM_PROXY_IDLE",
                "PM_PSRAM_PROXY_PREPARING",
                "PM_PSRAM_PROXY_WAIT_ACK",
                "PM_PSRAM_PROXY_DONE",
                "PM_PSRAM_PROXY_QUARANTINE",
                "s_pm_psram_proxy_state",
            )
        ),
        "ap-atomic-proxy-state-declares-preparing-wait-done-quarantine",
    )
    proxy_begin = extract_function(ap, "pm_psram_proxy_begin")
    proxy_complete = extract_function(ap, "pm_psram_proxy_complete")
    contract.require(
        has_order(
            proxy_begin,
            [
                r"PM_PSRAM_PROXY_IDLE",
                r"PM_PSRAM_PROXY_PREPARING",
                r"s_pm_psram_proxy_request",
                r"s_pm_psram_power_result\s*=\s*BK_FAIL",
                r"__atomic_store_n\s*\(\s*&s_pm_psram_proxy_state\s*,"
                r"\s*PM_PSRAM_PROXY_WAIT_ACK\s*,\s*__ATOMIC_RELEASE",
            ],
        ),
        "ap-claim-initialize-release-wait-before-send",
    )
    contract.require(
        has_order(
            ap_vote,
            [
                r"pm_psram_proxy_begin\s*\(",
                r"pm_cp1_mailbox_send_data\s*\(",
            ],
        ),
        "ap-send-occurs-only-after-wait-publication",
    )
    contract.require(
        re.search(
            r"state\s*==\s*PM_PSRAM_PROXY_PREPARING"
            r"[\s\S]*?(?:break\s*;|return\s*;)",
            ap_response,
        )
        is not None
        and ap_response.find("PM_PSRAM_PROXY_PREPARING")
        < ap_response.find("s_pm_psram_power_result"),
        "ap-isr-discards-response-during-preparing",
    )
    contract.require(
        "pm_psram_proxy_begin" in ap_vote
        and re.search(
            r"pm_psram_proxy_begin\s*\([^;]*\)\s*!=\s*BK_OK"
            r"[\s\S]*return\s+BK_FAIL",
            ap_vote,
        )
        is not None,
        "ap-concurrent-and-quarantined-callers-fail-before-send",
    )
    contract.require(
        "pm_psram_proxy_timeout" in ap_vote
        and has_order(
            ap_vote,
            [
                r"!\s*bk_pm_cp1_psram_power_state_get\s*\(\s*\)",
                r"pm_psram_proxy_timeout\s*\(",
                r"return\s+BK_FAIL",
            ],
        ),
        "ap-timeout-atomically-enters-quarantine",
    )
    contract.require(
        "pm_psram_proxy_ack" in ap_response
        and re.search(
            r"pm_psram_proxy_ack\s*\(\s*cmd_buf->param2\s*\)",
            ap_response,
        )
        is not None,
        "ap-ack-routes-through-cycle-state-machine",
    )
    proxy_helpers = [
        extract_function(ap, name)
        for name in (
            "pm_psram_proxy_begin",
            "pm_psram_proxy_timeout",
            "pm_psram_proxy_ack",
            "pm_psram_proxy_complete",
        )
    ]
    contract.require(
        all(body and len(body.splitlines()) <= 50 for body in proxy_helpers),
        "ap-new-proxy-functions-at-most-50-lines",
    )
    contract.require(
        has_order(
            proxy_complete,
            [
                r"(?:const\s+)?bk_err_t\s+(?:local_)?result\s*="
                r"\s*s_pm_psram_power_result",
                r"PM_PSRAM_PROXY_IDLE",
                r"return\s+(?:local_)?result",
            ],
        ),
        "ap-complete-captures-local-before-release-idle",
    )
    complete_call = ap_vote.find("pm_psram_proxy_complete(")
    after_complete = ap_vote[complete_call:] if complete_call >= 0 else ""
    contract.require(
        bool(after_complete)
        and "s_pm_psram_power_result" not in after_complete
        and re.search(r"ret\s*=\s*pm_psram_proxy_complete\s*\(", after_complete)
        is not None,
        "ap-caller-uses-only-local-completed-result",
    )
    contract.require(
        cp_power.count("bk_psram_init()") == 3,
        "cp-power-on-attempts-init-exactly-three-times",
    )
    contract.require(
        re.search(
            r"if\s*\(\s*ret\s*==\s*BK_OK\s*\)\s*\{[\s\S]*"
            r"s_pm_psram_ctrl_state\s*\|=",
            cp_power,
        )
        is not None
        and re.search(
            r"return\s+ret\s*;\s*\}",
            cp_power,
        )
        is not None,
        "cp-three-failures-do-not-set-bit-and-return-real-ret",
    )
    contract.require(
        bool(cp_power) and len(cp_power.splitlines()) <= 50,
        "cp-provider-function-at-most-50-lines",
    )
    contract.require(
        expected_ap_behavior(False, 0, True) == (-1, False)
        and expected_ap_behavior(True, -1, True) == (-1, False)
        and expected_ap_behavior(True, 0, True) == (0, True)
        and expected_ap_behavior(True, 0, False) == (0, False),
        "matched-provider-behavior-oracle",
    )

    if contract.missing:
        print(
            f"PM-05A:PROXY:FAIL checks={contract.checks} "
            f"missing={len(contract.missing)} oracle_cases=4",
            file=sys.stderr,
        )
        return 1
    print(
        f"PM-05A:PROXY:PASS checks={contract.checks} "
        "missing=0 oracle_cases=4"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
