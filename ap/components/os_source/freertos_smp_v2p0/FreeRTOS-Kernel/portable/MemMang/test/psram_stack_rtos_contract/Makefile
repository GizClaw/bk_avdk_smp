MEMMANG_DIR := $(abspath ../..)
TEST_DIR := $(CURDIR)
BUILD_DIR := $(TEST_DIR)/build
MODEL_TARGET := $(BUILD_DIR)/rtos_order_model_test
PM_LIFECYCLE_MODEL_TARGET := $(BUILD_DIR)/pm_lifecycle_model_test
PM_LIFECYCLE_ACTUAL_TARGET := $(BUILD_DIR)/pm_lifecycle_actual_test
PM05C_MODEL_TARGET := $(BUILD_DIR)/pm05c_transaction_model_test
PM05C_REVIEW2_MODEL_TARGET := $(BUILD_DIR)/pm05c_second_review_model_test
PM05C_FACADE_TARGET := $(BUILD_DIR)/pm05c_facade_actual_test
PM05C_FACADE_SOURCE := $(abspath ../../../../../../../../components/bk_rtos/freertos/v10/psram_stack_pm.c)
ARM_TARGET := $(BUILD_DIR)/rtos_contract_arm_probe.o
PM05A_TEST_DIR := $(abspath ../psram_stack_contract)
CACHE_INCLUDE_DIR := $(abspath $(MEMMANG_DIR)/../../../../../../middleware/arch/cm33/include)

CC ?= cc
ARM_CC ?= /opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wshadow
ARM_CFLAGS += -std=c11 -Wall -Wextra -Werror -Wshadow \
	-mcpu=cortex-m33 -mthumb
ARM_CPPFLAGS += -I$(MEMMANG_DIR) \
	-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1

.NOTPARALLEL:
.PHONY: verify model pm-lifecycle-actual pm-lifecycle-model \
	pm-facade-actual pm-tls-arm pm-lifecycle-source-contract \
	pm-lifecycle-arm pm05c-model pm05c-facade-actual \
	pm05c-provider-actual pm05c-source-contract pm05c-layout-arm \
	pm05c-review2-model pm05c-review2-contract pm05c-cp-destructive-actual \
	pm05c-integration-actual \
	pm05a-regression \
	fatal-reset-stub source-contract arm-compile arm-source clean

verify:
	$(MAKE) clean
	@status=0; \
	$(MAKE) model || status=1; \
	$(MAKE) pm-lifecycle-actual || status=1; \
	$(MAKE) pm-lifecycle-model || status=1; \
	$(MAKE) pm-facade-actual || status=1; \
	$(MAKE) pm-tls-arm || status=1; \
	$(MAKE) fatal-reset-stub || status=1; \
	$(MAKE) source-contract || status=1; \
	$(MAKE) pm-lifecycle-source-contract || status=1; \
	$(MAKE) arm-compile || status=1; \
	$(MAKE) arm-source || status=1; \
	$(MAKE) pm-lifecycle-arm || status=1; \
	$(MAKE) pm05c-model || status=1; \
	$(MAKE) pm05c-facade-actual || status=1; \
	$(MAKE) pm05c-provider-actual || status=1; \
	$(MAKE) pm05c-source-contract || status=1; \
	$(MAKE) pm05c-layout-arm || status=1; \
	$(MAKE) pm05c-review2-model || status=1; \
	$(MAKE) pm05c-review2-contract || status=1; \
	$(MAKE) pm05c-cp-destructive-actual || status=1; \
	$(MAKE) pm05c-integration-actual || status=1; \
	$(MAKE) pm05a-regression || status=1; \
	exit $$status

model: $(MODEL_TARGET)
	$(MODEL_TARGET)

pm-lifecycle-actual: $(PM_LIFECYCLE_ACTUAL_TARGET)
	$(PM_LIFECYCLE_ACTUAL_TARGET)

pm-lifecycle-model: $(PM_LIFECYCLE_MODEL_TARGET)
	$(PM_LIFECYCLE_MODEL_TARGET)

pm-facade-actual:
	python3 $(TEST_DIR)/pm_facade_actual_test.py

pm-tls-arm:
	python3 $(TEST_DIR)/pm_tls_arm_contract_test.py

pm-lifecycle-source-contract:
	python3 $(TEST_DIR)/pm_lifecycle_source_contract_test.py

pm-lifecycle-arm:
	python3 $(TEST_DIR)/pm_lifecycle_arm_contract_test.py

pm05c-model: $(PM05C_MODEL_TARGET)
	$(PM05C_MODEL_TARGET)

pm05c-facade-actual: $(PM05C_FACADE_TARGET)
	$(PM05C_FACADE_TARGET)

pm05c-provider-actual:
	python3 $(TEST_DIR)/pm05c_provider_actual_test.py

pm05c-source-contract:
	python3 $(TEST_DIR)/pm05c_source_contract_test.py

pm05c-layout-arm:
	python3 $(TEST_DIR)/pm05c_layout_arm_test.py

pm05c-review2-model: $(PM05C_REVIEW2_MODEL_TARGET)
	$(PM05C_REVIEW2_MODEL_TARGET)

pm05c-review2-contract:
	python3 $(TEST_DIR)/pm05c_second_review_contract_test.py

pm05c-cp-destructive-actual:
	python3 $(TEST_DIR)/pm05c_cp_destructive_actual_test.py

pm05c-integration-actual:
	python3 $(TEST_DIR)/pm05c_integration_actual_test.py

pm05a-regression:
	$(MAKE) -C $(PM05A_TEST_DIR) verify ITERATIONS=100000

fatal-reset-stub:
	python3 $(TEST_DIR)/fatal_reset_stub_test.py

source-contract:
	python3 $(TEST_DIR)/rtos_source_contract_test.py

arm-compile: $(ARM_TARGET)
	@echo "RTOS-04:ARM-FACADE:PASS"

arm-source:
	python3 $(TEST_DIR)/arm_feature_on_source_test.py

$(MODEL_TARGET): $(TEST_DIR)/rtos_order_model_test.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(PM_LIFECYCLE_MODEL_TARGET): $(TEST_DIR)/pm_lifecycle_model_test.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(PM_LIFECYCLE_ACTUAL_TARGET): $(TEST_DIR)/pm_lifecycle_actual_test.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DPSRAM_STACK_HOST_TEST=1 \
		-DCONFIG_MEM_DEBUG=1 -Wno-pointer-to-int-cast \
		-Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
		-I$(PM05A_TEST_DIR)/stubs -I$(CACHE_INCLUDE_DIR) -I$(MEMMANG_DIR) \
		-ffunction-sections -fdata-sections -pthread $< \
		-Wl,--gc-sections -pthread -o $@

$(PM05C_MODEL_TARGET): $(TEST_DIR)/pm05c_transaction_model_test.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(PM05C_REVIEW2_MODEL_TARGET): $(TEST_DIR)/pm05c_second_review_model_test.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -pthread $< -pthread -o $@

$(PM05C_FACADE_TARGET): $(TEST_DIR)/pm05c_facade_actual_test.c \
		$(PM05C_FACADE_SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(TEST_DIR)/pm05c_stubs \
		-DPM05C_FACADE_SOURCE='"$(PM05C_FACADE_SOURCE)"' $< -o $@

$(ARM_TARGET): $(TEST_DIR)/rtos_contract_arm_probe.c \
		$(MEMMANG_DIR)/psram_stack_allocator.h
	mkdir -p $(BUILD_DIR)
	@$(ARM_CC) $(ARM_CPPFLAGS) $(ARM_CFLAGS) -c $< -o $@ \
		>$(BUILD_DIR)/arm-facade.log 2>&1 || { \
		echo "RTOS-04:ARM-FACADE:FAIL allocator facade still exports owner span"; \
		$(ARM_CC) $(ARM_CPPFLAGS) $(ARM_CFLAGS) -c $< -o $@; \
		exit 1; \
	}

clean:
	rm -rf $(BUILD_DIR)
