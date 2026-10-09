# OTP2 配置与烧录指南（BK7258）

本文档说明 **BK7258** 平台 `secureboot_xxx` 工程中 OTP2 区域的配置方法、客户自定义空间的使用，以及如何通过 `otp_efuse_config.json` 由 bkfil 工具烧写进硬件。

---

## 1. 概述

- OTP2 是通过 **AHB** 访问的一次性可编程（OTP）存储区，总容量 `0xC00`（3072 字节）。
- OTP2 的条目布局由 `otp2.csv` 描述，编译时由 `tools/env_tools/beken_utils/scripts/gen_otp_map.py` 生成 `_otp.h` / `_otp.c`（`otp2_id_t`、`otp_map_2[]`）。
- OTP 特性：每个 bit 只能从 `0` 编程为 `1`，不可回写；未编程读回为全 `0`。

### 1.1 条目地址

BK7258 上某 OTP2 条目的烧录 / 调试地址：

```
物理地址 = 0x4B010000 + 该条目在 otp2.csv 中的 offset
```

示例：`offset = 0x600` → `start_addr = 0x4B010600`（与客户可操作区间文档一致）。

> OTP1（验签公钥哈希、Flash AES 密钥等）与 OTP2（AHB 校准 / 客户区）是不同接口：**不要**把 OTP1 的 `0x4B100xxx` 地址套用到 OTP2 自定义数据上。

---

## 2. OTP2 空间布局（BK7258）

| 区间（offset） | 物理地址（AHB） | 用途 | 建议 |
| --- | --- | --- | --- |
| `0x000` ~ `0x600` | `0x4B010000` ~ `0x4B010600` | 平台区（PHY、RF 校准、MAC 等）；硬件文档称前 1.5KB 为内部使用 | 不随意改动已有平台条目 |
| `0x600` ~ `0xC00` | `0x4B010600` ~ `0x4B010C00` | **客户自定义区**（1.5KB） | 按需划分条目 |

- `0x600` 为平台区与客户区的分界，且为 **256 字节对齐**（`0x600 = 6 × 0x100`）。
- 板级默认 `cp/middleware/boards/bk7258/csv/otp2.csv` 只占用 `0x000` ~ `0x460`。在客户区从 `0x600` 开始前，应增加 **`OTP_PLATFORM_RESERVED`**（或同类命名）条目，将 `offset` 从 `0x460` 连续填到 `0x600`，避免 `otp2.csv` 与硬件预留边界不一致。

---

## 3. 操作步骤

### 步骤 1：将 otp2.csv 拷贝到工程

以 board 目录下的标准表为基准，拷贝到工程分区目录后增加自定义条目（**工程副本优先**于 board 表参与 prebuild）：

```bash
cp cp/middleware/boards/bk7258/csv/otp2.csv \
   projects/secureboot_xip/partitions/bk7258/otp2.csv
```

AP 侧编译使用otp2时，需要将源文件 `otp2.csv`放在ap侧（布局应与 CP 保持一致）。

之后所有修改均在工程副本 `projects/secureboot_xip/partitions/bk7258/otp2.csv` 上进行。

### 步骤 2：补齐保留区并在 `0x600` 起分配客户条目

在默认 9 条平台条目（`id` 0~8，结束于 `0x460`）之后追加：

#### 2.1 CSV 字段说明

```
id,   name,   size,   offset,   end,   privilege,        security
```

| 字段 | 含义 |
| --- | --- |
| `id` | 从 0 连续递增，不可跳号 |
| `name` | 生成 `otp2_id_t` 枚举成员 |
| `size` | 字节长度（十进制） |
| `offset` / `end` | 十六进制，须满足 `size == end - offset` |
| `privilege` | `OTP_READ_WRITE` / `OTP_READ_ONLY` / `OTP_NO_ACCESS` |
| `security` | `TRUE` = 安全，`FALSE` = 非安全 |

#### 2.2 生成脚本校验规则

1. `id` 连续；
2. 本条 `offset` ≥ 上条 `end`（不重叠、不回退）；
3. `size == end - offset`。

#### 2.3 示例：保留区 + 客户密钥 + 尾部保留（填满至 `0xC00`）

在板级 0~8 行之后追加（`id` 9、10、11）：

```csv
9,      OTP_PLATFORM_RESERVED,          416,        0x460,         0x600,       OTP_READ_WRITE,           FALSE,
10,     OTP_CUSTOMER_KEY,               32,         0x600,         0x620,       OTP_READ_WRITE,           FALSE,
11,     OTP_CUSTOMER_RESERVED,          1504,       0x620,         0xC00,       OTP_READ_WRITE,           FALSE,
```

说明：

- `OTP_PLATFORM_RESERVED`：`0x600 - 0x460 = 0x1A0`（416 字节），保证表内 offset 连续到客户区起点。
- `OTP_CUSTOMER_KEY`：客户区首条目，位于 `0x600`，32 字节；枚举值为 **10**（与 `id` 一致）。
- `OTP_CUSTOMER_RESERVED`：占满 `0x620` ~ `0xC00`，便于后续再切分或保持 `id` 连续。

修改后重新编译工程，在 `.../armino/partitions/_build/_otp.h` 中确认已生成上述枚举名。

### 步骤 3：配置 otp_efuse_config.json（OTP2 客户数据）

bkfil 从打包目录读取 `otp_efuse_config.json`，例如：

`projects/secureboot_xip/build/bk7258/secureboot_xip/package/otp_efuse_config.json`

- **OTP1 安全数据**（`flash_aes_key`、`bl1_rotpk_hash` 等）由 `security.csv` 与签名流程自动生成，地址在 **`0x4B100xxx`**（APB）。
- **OTP2 客户区**需在 `Security_Data` 中 **手工追加**（或维护单独的烧录 JSON），地址使用 **`0x4B010000 + offset`**。

#### 3.1 仅追加 OTP2 客户密钥的 JSON 片段

在已有 `Security_Data` 数组末尾增加（与上例 `OTP_CUSTOMER_KEY` 对应）：

```json
{
    "name": "customer_key_otp2",
    "mode": "write",
    "permission": "RO",
    "start_addr": "0x4B010600",
    "last_valid_addr": "0x4B010620",
    "byte_len": "0x20",
    "data": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "data_type": "hex",
    "status": "true"
}
```

字段计算：

| 字段 | 取值 | 说明 |
| --- | --- | --- |
| `start_addr` | `0x4B010000 + offset` | `0x600` → **`0x4B010600`** |
| `last_valid_addr` | `start_addr + byte_len` | `0x4B010600 + 0x20` → **`0x4B010620`** |
| `byte_len` | 十六进制 | 32 字节 → `0x20` |
| `data` | hex 字符串 | 实际烧写内容 |
| `permission` | `RO` / `WR` | 量产常对密钥使用 `RO` |

#### 3.2 完整示例（含 Secure Boot OTP1 + OTP2 客户密钥）

以下为 **结构示例**（OTP1 三项地址与密钥需与当前工程 `security.csv` / 签名结果一致；OTP2 段对应上文 CSV）：

```json
{
    "User_Operate_Enable": "false",
    "Security_Ctrl_Enable": "true",
    "Security_Data_Enable": "true",
    "User_Operate": [],
    "Security_Ctrl": [
        {
            "secure_boot_enable": "0,0,1",
            "secure_boot_debug_disable": "0,1,0",
            "fast_boot_disable": "0,2,0",
            "boot_mode": "0,3,1",
            "secure_boot_clock_select": "0,4,0",
            "random_delay_enable": "0,5,0",
            "direct_jump_enable": "0,6,1",
            "boot_critical_error": "0,7,0",
            "attack_nmi_enable": "2,4,0",
            "spi_to_ahb_disable": "2,5,0",
            "auto_reset_enable[0]": "2,6,0",
            "auto_reset_enable[1]": "2,7,0",
            "flash_aes_enable": "3,5,1",
            "spi_download_disable": "3,6,0",
            "swd_disable": "3,7,0"
        }
    ],
    "Security_Data": [
        {
            "name": "flash_aes_key",
            "mode": "write",
            "permission": "RO",
            "start_addr": "0x4B100460",
            "last_valid_addr": "0x4B100480",
            "byte_len": "0x20",
            "data": "73c7bf397f2ad6bf4e7403a7b965dc5ce0645df039c2d69c814ffb403183fb18",
            "data_type": "hex",
            "status": "true"
        },
        {
            "name": "bl1_rotpk_hash",
            "mode": "write",
            "permission": "RO",
            "start_addr": "0x4B100528",
            "last_valid_addr": "0x4B100548",
            "byte_len": "0x20",
            "data": "2af1f2236872981bdf2f17975fb3029fad171de61741a383ef2c621fd5f5cf62",
            "data_type": "hex",
            "status": "true"
        },
        {
            "name": "bl2_rotpk_hash",
            "mode": "write",
            "permission": "RO",
            "start_addr": "0x4B100548",
            "last_valid_addr": "0x4B100568",
            "byte_len": "0x20",
            "data": "ca68e9b3e991433ca9a6a2d6a74fc9dd5ccf559a4ae179537bde2772e0393ccf",
            "data_type": "hex",
            "status": "true"
        },
        {
            "name": "customer_key_otp2",
            "mode": "write",
            "permission": "RO",
            "start_addr": "0x4B010600",
            "last_valid_addr": "0x4B010620",
            "byte_len": "0x20",
            "data": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "data_type": "hex",
            "status": "true"
        }
    ]
}
```

---

## 4. 烧录与验证

### 4.1 烧录

使用 bkfil 加载 `otp_efuse_config.json`，将固件与 OTP 数据一并烧入后复位。

### 4.2 CLI 回读验证

在配置中开启 OTP 测试（若工程已支持）：

```
CONFIG_OTP_V1=y
CONFIG_OTP_TEST=y
```

控制台命令（`item_id` 为 `otp2_id_t` 枚举值，十进制；`size` 为字节数）：

```
otp_ahb read  <item_id> <size>
otp_ahb write <item_id> <size> <hex>
otp_ahb read_permission <item_id>
```

示例：读取 `OTP_CUSTOMER_KEY`（上例 `id = 10`，32 字节）：

```
otp_ahb read 10 32
```

读通路日志中的访问地址应落在 **`0x4B010600`** 一带（与 `SOC_ADDR_OFFSET` 一致时即为 `0x4B010600 + offset`），可与 JSON 中 `start_addr` 对照。

---

## 5. 注意事项

1. OTP 一次性编程：量产前务必样片 `read` 确认原内容为全 `0`。
2. `otp_efuse_config.json` 在 build/package 目录；烧录前确认已包含 OTP2 客户条目，或单独保存烧录用 JSON。
3. 规划遵循：**id 连续、offset 不回退、`size = end - offset`、安全/非安全 256 字节对齐**；BK7258 须在 `0x460`~`0x600` 用保留条目填满。
4. 工程级 `otp2.csv` 路径：`projects/secureboot_xxx/partitions/bk7258/otp2.csv`；修改后若 `_otp.h` 未更新，可 clean 后全量编译（partitions 的 CMake 依赖未单独列出 `otp2.csv`）。
5. 与 BK7259 混用地址时勿照搬 `0x42010xxx` / `0x52010xxx`，BK7258 OTP2 统一使用 **`0x4B010xxx`**。
