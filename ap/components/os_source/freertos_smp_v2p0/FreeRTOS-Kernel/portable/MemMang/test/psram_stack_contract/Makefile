MEMMANG_DIR := $(abspath ../..)
TEST_DIR := $(CURDIR)
BUILD_DIR := $(TEST_DIR)/build
HEAP_SOURCE := $(MEMMANG_DIR)/heap_4.c
CACHE_INCLUDE_DIR := $(abspath $(MEMMANG_DIR)/../../../../../../middleware/arch/cm33/include)
TARGET := $(BUILD_DIR)/psram_stack_contract_test
PM_MODEL_TARGET := $(BUILD_DIR)/pm_accounting_model_test
PM_ACTUAL_TARGET := $(BUILD_DIR)/pm_actual_api_contract_test
PM_FEATURE_OFF_TARGET := $(BUILD_DIR)/pm_feature_off_actual_test
PM_PROXY_MODEL_TARGET := $(BUILD_DIR)/pm_power_proxy_state_model_test
ARM_HEAP_TARGET := $(BUILD_DIR)/heap_4_actual_arm.o
ARM_FEATURE_OFF_HEAP_TARGET := $(BUILD_DIR)/heap_4_feature_off_actual_arm.o
ARM_PROBE_TARGET := $(BUILD_DIR)/allocator_contract_arm_probe.o
ASSERT_SELF_TEST_LOG := $(BUILD_DIR)/assert_count_self_test.log

CC ?= cc
ARM_CC ?= /opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wshadow \
	-Wno-pointer-to-int-cast -Wno-unused-function \
	-Wno-unused-parameter -Wno-sign-compare \
	-ffunction-sections -fdata-sections -pthread
CPPFLAGS += -I$(TEST_DIR)/stubs \
	-I$(CACHE_INCLUDE_DIR) -I$(MEMMANG_DIR) \
	-DITERATIONS=$(ITERATIONS) -D_GNU_SOURCE -DPSRAM_STACK_HOST_TEST=1 \
	-DCONFIG_MEM_DEBUG=1
LDFLAGS += -Wl,--gc-sections -pthread
ARM_CFLAGS += -std=c11 -Wall -Wextra -Werror -Wshadow \
	-Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
	-mcpu=cortex-m33 -mthumb
ARM_CPPFLAGS += -I$(TEST_DIR)/stubs \
	-I$(CACHE_INCLUDE_DIR) -I$(MEMMANG_DIR) \
	-DCONFIG_MEM_DEBUG=1
ITERATIONS ?= 100000

SOURCES := \
	$(TEST_DIR)/allocator_contract_test.c

.NOTPARALLEL:

.PHONY: verify test pm-source-contract pm-proxy-contract assert-count-self-test arm-compile \
	linker-negative clean

verify:
	$(MAKE) clean
	$(MAKE) arm-compile
	$(MAKE) assert-count-self-test ITERATIONS=$(ITERATIONS)
	@status=0; \
	$(MAKE) test ITERATIONS=$(ITERATIONS) || status=$$?; \
	$(MAKE) linker-negative || status=$$?; \
	exit $$status

test: $(TARGET) $(PM_MODEL_TARGET) $(PM_ACTUAL_TARGET) \
		$(PM_FEATURE_OFF_TARGET) $(PM_PROXY_MODEL_TARGET)
	@status=0; \
	$(TARGET) || status=1; \
	$(PM_MODEL_TARGET) || status=1; \
	$(PM_ACTUAL_TARGET) || status=1; \
	$(PM_FEATURE_OFF_TARGET) || status=1; \
	$(PM_PROXY_MODEL_TARGET) || status=1; \
	$(MAKE) pm-source-contract || status=1; \
	$(MAKE) pm-proxy-contract || status=1; \
	exit $$status

pm-source-contract:
	python3 $(TEST_DIR)/pm_source_contract_test.py

pm-proxy-contract:
	python3 $(TEST_DIR)/pm_power_proxy_contract_test.py

assert-count-self-test: $(TARGET)
	@status=0; \
	$(TARGET) --assert-count-self-test >$(ASSERT_SELF_TEST_LOG) 2>&1 \
		|| status=$$?; \
	if [ "$$status" -ne 1 ] \
		|| ! awk '/^CACHEALLOC-02:ALLOC:FAIL .*asserts=1 lock_errors=0$$/ \
			{ found = 1 } END { exit !found }' $(ASSERT_SELF_TEST_LOG); then \
		awk '{ print }' $(ASSERT_SELF_TEST_LOG); \
		exit 1; \
	fi
	@echo "CACHEALLOC-02:ASSERT-COUNT:PASS"

arm-compile:
	$(MAKE) $(ARM_HEAP_TARGET)
	$(MAKE) $(ARM_FEATURE_OFF_HEAP_TARGET)
	$(MAKE) $(ARM_PROBE_TARGET)

linker-negative:
	python3 $(TEST_DIR)/linker_layout_contract_test.py

$(TARGET): $(SOURCES) $(HEAP_SOURCE) $(wildcard $(TEST_DIR)/stubs/*) \
		$(MEMMANG_DIR)/psram_stack_allocator.h \
		$(CACHE_INCLUDE_DIR)/cache_psram_stack_internal.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $(TARGET)

$(PM_MODEL_TARGET): $(TEST_DIR)/pm_accounting_model_test.c
	mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -Wshadow $< -o $@

$(PM_ACTUAL_TARGET): $(TEST_DIR)/pm_actual_api_contract_test.c \
		$(TEST_DIR)/allocator_contract_test.c $(HEAP_SOURCE) \
		$(MEMMANG_DIR)/psram_stack_allocator.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

$(PM_FEATURE_OFF_TARGET): $(TEST_DIR)/pm_feature_off_actual_test.c \
		$(HEAP_SOURCE) $(wildcard $(TEST_DIR)/stubs/*)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=0 \
		$< $(LDFLAGS) -o $@

$(PM_PROXY_MODEL_TARGET): $(TEST_DIR)/pm_power_proxy_state_model_test.c
	mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -Wshadow $< -o $@

$(ARM_HEAP_TARGET): $(HEAP_SOURCE) $(wildcard $(TEST_DIR)/stubs/*)
	mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(ARM_CPPFLAGS) $(ARM_CFLAGS) \
		-c $(HEAP_SOURCE) -o $(ARM_HEAP_TARGET)

$(ARM_FEATURE_OFF_HEAP_TARGET): $(HEAP_SOURCE) $(wildcard $(TEST_DIR)/stubs/*)
	mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(ARM_CPPFLAGS) $(ARM_CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=0 \
		-c $(HEAP_SOURCE) -o $(ARM_FEATURE_OFF_HEAP_TARGET)

$(ARM_PROBE_TARGET): $(TEST_DIR)/allocator_contract_arm_probe.c
	mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(ARM_CPPFLAGS) $(ARM_CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1 \
		-DPSRAM_STACK_ALLOCATOR_ARM_PROBE=1 \
		-c $(TEST_DIR)/allocator_contract_arm_probe.c -o $(ARM_PROBE_TARGET)

clean:
	rm -rf $(BUILD_DIR)
