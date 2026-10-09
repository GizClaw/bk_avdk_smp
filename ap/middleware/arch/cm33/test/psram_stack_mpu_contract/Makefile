TEST_DIR := $(CURDIR)
AP_DIR := $(abspath $(TEST_DIR)/../../../../..)
MPU_SOURCE := $(AP_DIR)/middleware/soc/bk7258_ap/mpu_cfg.c
BUILD_DIR := $(TEST_DIR)/build
FEATURE_ON_TARGET := $(BUILD_DIR)/mpu_feature_on_test
LOWER_EDGE_TARGET := $(BUILD_DIR)/mpu_lower_edge_test
UPPER_EDGE_TARGET := $(BUILD_DIR)/mpu_upper_edge_test
DCACHE_OFF_TARGET := $(BUILD_DIR)/mpu_dcache_off_test
FEATURE_OFF_TARGET := $(BUILD_DIR)/mpu_feature_off_test
FEATURE_OFF_DCACHE_TARGET := $(BUILD_DIR)/mpu_feature_off_dcache_test
ARM_TARGET := $(BUILD_DIR)/mpu_cfg_feature_on_arm.o

CC ?= cc
ARM_CC ?= /opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wshadow
CPPFLAGS += -I$(TEST_DIR)/stubs
ARM_CFLAGS += -std=c11 -Wall -Wextra -Werror -Wshadow \
	-mcpu=cortex-m33 -mthumb
ARM_CPPFLAGS += -I$(TEST_DIR)/stubs \
	-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1 -DCONFIG_DCACHE=1

.NOTPARALLEL:
.PHONY: verify test arm-compile clean

verify:
	$(MAKE) clean
	@status=0; \
	$(MAKE) test || status=1; \
	$(MAKE) arm-compile || status=1; \
	exit $$status

test: $(FEATURE_ON_TARGET) $(LOWER_EDGE_TARGET) $(UPPER_EDGE_TARGET) \
		$(DCACHE_OFF_TARGET) $(FEATURE_OFF_TARGET) \
		$(FEATURE_OFF_DCACHE_TARGET)
	@status=0; \
	$(FEATURE_ON_TARGET) || status=1; \
	$(LOWER_EDGE_TARGET) || status=1; \
	$(UPPER_EDGE_TARGET) || status=1; \
	$(DCACHE_OFF_TARGET) || status=1; \
	$(FEATURE_OFF_TARGET) || status=1; \
	$(FEATURE_OFF_DCACHE_TARGET) || status=1; \
	exit $$status

arm-compile: $(ARM_TARGET)
	@echo "RTOS-04:MPU-ARM:PASS"

$(FEATURE_ON_TARGET): $(TEST_DIR)/mpu_contract_test.c $(MPU_SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1 -DCONFIG_DCACHE=1 \
		-DRTOS04_MPU_CASE_NAME=\"feature-on-interior\" \
		$^ -o $@

$(LOWER_EDGE_TARGET): $(TEST_DIR)/mpu_contract_test.c $(MPU_SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1 -DCONFIG_DCACHE=1 \
		-DCONFIG_AP_PSRAM_STACK_HEAP_ADDR=0x60000000u \
		-DRTOS04_MPU_CASE_NAME=\"zero-leading\" \
		$^ -o $@

$(UPPER_EDGE_TARGET): $(TEST_DIR)/mpu_contract_test.c $(MPU_SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1 -DCONFIG_DCACHE=1 \
		-DCONFIG_AP_PSRAM_STACK_HEAP_ADDR=0x63f00000u \
		-DRTOS04_MPU_CASE_NAME=\"zero-trailing-upper-edge\" \
		$^ -o $@

$(DCACHE_OFF_TARGET): $(TEST_DIR)/mpu_contract_test.c $(MPU_SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1 -DCONFIG_DCACHE=0 \
		-DRTOS04_MPU_CASE_NAME=\"feature-on-dcache-off\" \
		$^ -o $@

$(FEATURE_OFF_TARGET): $(TEST_DIR)/mpu_contract_test.c $(MPU_SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=0 -DCONFIG_DCACHE=0 \
		-DRTOS04_MPU_CASE_NAME=\"feature-off-dcache-off\" \
		$^ -o $@

$(FEATURE_OFF_DCACHE_TARGET): $(TEST_DIR)/mpu_contract_test.c $(MPU_SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=0 -DCONFIG_DCACHE=1 \
		-DRTOS04_MPU_CASE_NAME=\"feature-off-dcache-on\" \
		$^ -o $@

$(ARM_TARGET): $(MPU_SOURCE)
	mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(ARM_CPPFLAGS) $(ARM_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
