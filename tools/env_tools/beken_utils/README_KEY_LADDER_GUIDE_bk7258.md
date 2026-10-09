# Key Ladder 使用指导

## 1. 概述

Key Ladder 以 OTP 中的根密钥(model key/device root key)为起点，结合公开参数 EK1 / EK2 / EK3，在密码引擎内部派生出会话密钥，用于 AES-GCM 加解密。派生密钥不离开硬件，软件侧只配置根类型与 EK 参数。

当前软件接口：

- `bk_key_ladder_gcm_encrypt()` / `bk_key_ladder_gcm_decrypt()`
- 头文件：`ap/components/bk_trustengine/key_ladder_gcm.h`
- 实现与默认 EK：`ap/components/bk_trustengine/key_ladder_gcm.c`

使能条件（AP 侧）：

```
CONFIG_TRUSTENGINE=y        # 依赖硬件加速引擎
CONFIG_TRUSTENGINE_TEST=y   # 需要 CLI 自测时打开
```

CLI 自测：

```
mbedtls_key_ladder model
mbedtls_key_ladder device
mbedtls_key_ladder all
```

---

## 2. Model Key 与 Device Root Key 差异


| 项目      | Model Key                       | Device Root Key                       |
| ------- | ------------------------------- | ------------------------------------- |
| 来源      | 产线/研发通过 JSON 写入 OTP             | 芯片侧 device root（通常与 PUF 相关）           |
| 是否可人为指定 | 可以（随机生成后烧写）                     | 芯片出厂时由PUF自动生成                   |
| 同型号芯片   | 可共用同一 model key                 | 每颗芯片不同                                |
| 典型用途    | 同系列产品共享派生密钥体系       | 设备唯一、防克隆、单机绑定                       |
| API 选择  | `MBEDTLS_GCM_DERIVED_MODEL_KEY` | `MBEDTLS_GCM_DERIVED_DEVICE_ROOT_KEY` |
| CLI     | `mbedtls_key_ladder model`      | `mbedtls_key_ladder device`           |

使用建议：

1. **业务数据需跨多台设备用同一密钥解密** → 使用 model key。
2. **业务数据必须绑定到单台设备** → 使用 device root key。
3. 两种根密钥派生结果不同，密文不可互换。
4. Device root 依赖芯片 PUF/enrollment 状态,出厂时已完成初始化。

OTP1 布局中相关项（offset 相对 OTP1 基址 `0x4B100400`）：


| 名称                    | offset  | 长度   | 烧录地址                        |
| --------------------- | ------- | ---- | --------------------------- |
| `OTP_MODEL_KEY`       | `0x104` | 16 B | `0x4B100504` ~ `0x4B100514` |
| `OTP_DEVICE_ROOT_KEY` | `0x118` | 16 B | `0x4B100518` ~ `0x4B100528` |


> Device root key一般由芯片机制管理，本文只说明 **model key 的 JSON 烧写**。

---

## 3. 密钥与参数关系

```
          ┌─────────────┐
          │  Root Key   │  model key 或 device root key（OTP，硬件内使用）
          └──────┬──────┘
                 │  AES-ECB ladder
     EK3 ────────┤
     EK2 ────────┤
     EK1 ────────┤
                 ▼
          ┌─────────────┐
          │  Boot Key   │  派生会话密钥（AES-256，硬件内使用，不导出）
          └──────┬──────┘
                 │  AES-GCM
                 ▼
            cipher / tag
```

参数约定：

Model key方式:

| 参数            | 长度          | 如何得到                                           | 是否秘密           |
| ------------- | ----------- | ---------------------------------------------- | -------------- |
| **Model key** | 128 bit     | **随机数**，写入 OTP                                 | 是（同型号可共享）      |
| **EK2**       | 128 bit     | **随机数**                                        | 非根密钥，但仍建议妥善保管  |
| **EK3**       | 128 bit     | **随机数**                                        | 同上             |
| **EK1**       | 128/256 bit | **由脚本根据 model key + 目标 boot key + EK2/EK3 反推** | 可放代码/Flash/OTP |
| **Boot key**  | 与 EK1 同宽   k | 业务目标派生密钥（如 `00..1f`）                           | 是（仅存在于引擎内）     |



脚本在计算前会对 model key 做 **按 32-bit 字内小端转换**，以匹配 OTP 在硬件中的使用字节序。JSON 的 `data` 字段填写 **烧录字面值（大端 hex 字符串）**，无需预先手工翻成小端。

---

Device root key方式:

 Device root key在芯片内已随机生成,不需要使用脚本生成EK1;使用时需要更新SDK固件中EK1/2/3,由此派生的Boot key每一个设备都不相同.


## 4. 脚本使用说明

路径：`key_ladder/dubhe_gen_img_enc_key.py`

依赖：`pycryptodome`（`from Crypto.Cipher import AES`）

脚本内固定了默认 EK2/EK3（与代码默认值一致）。若更换随机 EK2/EK3，需同步修改脚本常量与固件中的 EK2/EK3。

### 4.1 反推 EK1（常用）

已知：随机 model key、随机 EK2/EK3、期望的 boot key → 生成 EK1。

```bash
# 第 4 个参数任意非空即可走 gen_ek1 分支
python3 dubhe_gen_img_enc_key.py <model_key_hex> <boot_key_hex> gen_ek1
```

示例：

```bash
python3 dubhe_gen_img_enc_key.py \
  3864145fb6d16f0e6c39ea191dbc1720 \
  000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f \
  gen_ek1
```

输出为 EK1（hex）。将其配置到固件（见第 6 节）。

约束：

- `model_key`：32 个 hex 字符（16 字节）
- `boot_key` / key blob：32 或 64 个 hex 字符（16 或 32 字节）



### 4.2 正推派生密钥（校验）

已知：model key、EK1 → 计算派生出的 boot key，用于核对。

```bash
python3 dubhe_gen_img_enc_key.py <model_key_hex> <ek1_hex>
```

### 4.3 推荐工作流

1. 生成随机 **model key**（16 字节）、随机 **EK2/EK3**（各 16 字节）。
2. 选定业务 **boot key**（建议 32 字节，避免 ECB 下两半相同）。
3. 将 EK2/EK3 写入脚本常量与固件配置源。
4. 用脚本生成 **EK1**，写入固件（或 Flash/OTP，见第 6 节）。
5. 将 model key 加入 `otp_efuse_config.json` 并烧写。
6. 编译烧录固件后，用 `mbedtls_key_ladder model` 做功能确认。

随机数示例（Linux）：

```bash
openssl rand -hex 16   # model key / ek2 / ek3
openssl rand -hex 32   # boot key（256-bit）
```

---

## 5. Model Key JSON 烧写

###  追加到现有 `otp_efuse_config.json`

打包目录中的完整配置示例：

`projects/<project>/build/.../package/otp_efuse_config.json`

该文件通常已包含 `flash_aes_key`、`bl1_rotpk_hash`、`bl2_rotpk_hash` 等。在 `Security_Data` **数组末尾追加** model key 一项即可，不要删掉原有安全项。

追加前（示意）：

```json
"Security_Data": [
    { "name": "flash_aes_key", "...": "..." },
    { "name": "bl1_rotpk_hash", "...": "..." },
    { "name": "bl2_rotpk_hash", "...": "..." }
]
```

追加后：

```json
"Security_Data": [
    { "name": "flash_aes_key", "...": "..." },
    { "name": "bl1_rotpk_hash", "...": "..." },
    { "name": "bl2_rotpk_hash", "...": "..." },
    {
        "name": "model_key",
        "mode": "write",
        "permission": "NA",
        "start_addr": "0x4B100504",
        "last_valid_addr": "0x4B100514",
        "byte_len": "0x10",
        "data": "<你的16字节model_key_hex>",
        "data_type": "hex",
        "status": "true"
    }
]
```

同时确认：

```json
"Security_Data_Enable": "true"
```

注意：

1. "permission"设置为NA后cpu将无法访问该密钥。
2. 重新打包工程可能覆盖 `package/otp_efuse_config.json`，量产应以可追溯的输入配置维护 model key，或每次打包后检查并补回该条目。
3. 仅烧 model key、不动 eFuse 控制位时，可将 `Security_Ctrl_Enable` 设为 `"false"`，避免误改安全开关。
4. JSON 标准不支持注释；若文件末尾有 `# ...` 说明行，烧录工具需能容忍，否则请删掉注释行。

---

## 6. EK1 / EK2 / EK3 配置方式

三者均可按产品安全与运维需求选择存放位置。当前参考实现将三者放在代码常量中。

### 6.1 方式 A：固化在代码中（当前默认）

文件：`key_ladder_gcm.c`

```c
static const uint8_t g_ek1[32] = { /* 脚本生成 */ };
static const uint8_t g_ek2[16] = { /* 随机数 */ };
static const uint8_t g_ek3[16] = { /* 随机数 */ };
```

`mbedtls_gcm_set_derived_key(..., g_ek1, g_ek2, g_ek3, 256)` 使用上述常量。

特点：实现简单；更换 EK 需重编固件；EK 出现在固件镜像中。

### 6.2 方式 B：从 Flash 分区读取

data分区中存储。

### 6.3 方式 C：从 OTP 读取

适用：EK 与 model key 一样要求一次性、抗篡改。

建议：

1. 在 `otp2.csv` 中规划空闲项。
2. 通过 `otp_efuse_config.json` 的 `Security_Data` 烧写 EK。
3. 运行时用 OTP 读接口取出后送入 key ladder。

无论哪种方式：**EK2/EK3/model key 变更后都必须用脚本重新生成 EK1**，并保证固件使用的 EK2/EK3 与脚本一致。

---

## 7. 软件调用示例

```c
#include "key_ladder_gcm.h"

uint8_t iv[KEY_LADDER_GCM_IV_LEN];
uint8_t tag[KEY_LADDER_GCM_TAG_LEN];
uint8_t cipher[plain_len];

/* 同型号共享密钥 */
int ret = bk_key_ladder_gcm_encrypt(
    MBEDTLS_GCM_DERIVED_MODEL_KEY,
    iv, sizeof(iv),
    aad, aad_len,
    plain, plain_len,
    cipher, tag, sizeof(tag));

/* 单机绑定 */
ret = bk_key_ladder_gcm_encrypt(
    MBEDTLS_GCM_DERIVED_DEVICE_ROOT_KEY,
    iv, sizeof(iv),
    aad, aad_len,
    plain, plain_len,
    cipher, tag, sizeof(tag));
```

返回值：`KEY_LADDER_OK` / `KEY_LADDER_ERR_PARAM` / `KEY_LADDER_ERR_ENGINE` / `KEY_LADDER_ERR_AUTH`。

IV 在同一派生密钥下必须唯一；AAD 可为空；`plain_len == 0` 时仅做认证（GMAC）。

---

## 8. 自测与预期核对

测试向量（CLI 内置）：

- IV：`00112233445566778899aabb`
- AAD：`key-ladder-gcm-headr`
- plain：32 字节 `0xA5`

当 boot key 为 `00010203...1e1f`，且 model key / EK 与脚本一致时，model 路径期望：

```
cipher: 7f235516eac2eebbcdc565b9df2927989ca716f0b7cfde122f2ab3553415bb1e
tag   : fb2d8ef5a94aa91084f798eaa51a1fd5
gmac  : dcc100c71840354f30c479d06d7cc83a
```

若 cipher/tag 与脚本正推结果不一致，优先检查：

1. OTP model key 是否已烧写且地址正确
2. 固件 EK1/EK2/EK3 是否与脚本同一套
3. 是否误用了 device 路径

---

## 9. 检查清单

- [ ] 已生成随机 model key、EK2、EK3  
- [ ] 已选定 boot key，并用脚本生成 EK1  
- [ ] 固件中 EK1/EK2/EK3 已按选定方式配置（代码 / Flash / OTP）  
- [ ] `otp_efuse_config.json` 已追加 `model_key` 且 `Security_Data_Enable=true`  
- [ ] AP 已打开 `CONFIG_TRUSTENGINE`  
- [ ] 烧写 OTP 与固件后，`mbedtls_key_ladder model` 通过  
- [ ] 需要单机绑定时，额外验证 `mbedtls_key_ladder device`（依赖 device root 状态）  