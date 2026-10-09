ARCH_DIR := $(abspath ../..)
TEST_DIR := $(CURDIR)
BUILD_DIR := $(TEST_DIR)/build
TARGET := $(BUILD_DIR)/cache_contract_test
ARM_CACHE_TARGET := $(BUILD_DIR)/cache_arm.o
ARM_PROBE_TARGET := $(BUILD_DIR)/cache_contract_arm_probe.o
AP_DIR := $(abspath $(ARCH_DIR)/../../..)
CMSIS_ROOT := $(AP_DIR)/components/cmsis/CMSIS_5
CMSIS_DEVICE_DIR := $(CMSIS_ROOT)/Device/Beken/armstar
CMSIS_CORE_DIR := $(CMSIS_ROOT)/CMSIS/Core/Include

CC ?= cc
ARM_CC ?= /opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wshadow
CPPFLAGS += -I$(TEST_DIR)/stubs -I$(ARCH_DIR)/include
ARM_CFLAGS += -std=c11 -Wall -Wextra -Werror -mcpu=cortex-m33 -mthumb
ARM_CPPFLAGS += \
	-I$(CMSIS_DEVICE_DIR) \
	-I$(CMSIS_CORE_DIR) \
	-I$(TEST_DIR)/stubs \
	-I$(ARCH_DIR)/include

SOURCES := \
	$(ARCH_DIR)/cache.c \
	$(TEST_DIR)/cache_contract_test.c

HEADERS := \
	$(TEST_DIR)/stubs/armstar.h \
	$(TEST_DIR)/stubs/mpu.h \
	$(TEST_DIR)/stubs/sdkconfig.h \
	$(ARCH_DIR)/include/cache.h

.PHONY: verify test arm-compile clean

verify:
	$(MAKE) clean
	$(MAKE) test
	$(MAKE) arm-compile

test: $(TARGET)
	$(TARGET)

arm-compile:
	$(MAKE) $(ARM_CACHE_TARGET)
	$(MAKE) $(ARM_PROBE_TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $(TARGET)

$(ARM_CACHE_TARGET): $(ARCH_DIR)/cache.c $(HEADERS) \
		$(CMSIS_DEVICE_DIR)/armstar.h $(CMSIS_CORE_DIR)/core_star.h
	mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(ARM_CPPFLAGS) $(ARM_CFLAGS) -c $(ARCH_DIR)/cache.c -o $(ARM_CACHE_TARGET)

$(ARM_PROBE_TARGET): $(TEST_DIR)/cache_contract_arm_probe.c
	mkdir -p $(BUILD_DIR)
	$(ARM_CC) $(ARM_CPPFLAGS) $(ARM_CFLAGS) \
		-DCONFIG_CACHEABLE_PSRAM_TASK_STACK=1 -DCACHE_CONTRACT_ARM_PROBE=1 \
		-c $(TEST_DIR)/cache_contract_arm_probe.c -o $(ARM_PROBE_TARGET)

clean:
	rm -rf $(BUILD_DIR)
