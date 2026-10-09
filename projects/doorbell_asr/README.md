# BK7258 Doorbell ASR Example

* [中文](./README_CN.md)

## 1. Overview

`doorbell_asr` is a BK7258 AP/CP smart-doorbell example that integrates Wanson
ASR with the doorbell audio and video framework. It supports:

- offline speech recognition with Wanson ASR;
- BLE network provisioning;
- LAN UDP and TCP doorbell services;
- the CS2 P2P doorbell service;
- onboard or UAC audio and voice communication;
- LCD display and the multimedia framework.

In the default configuration, ASR runs on the AP and starts automatically.
Wi-Fi and the BLE Controller run on the CP, while the BLE Host, doorbell
application, and multimedia services run on the AP.

## 2. Hardware Requirements

- BK7258 development board;
- 8 MB PSRAM;
- UAC microphone, which is the default ASR input;
- Wi-Fi and BLE antennas;
- CP UART0 console at 115200 baud;
- optional speaker, UVC/DVP camera, and LCD.

See the following files for GPIO configuration:

- `ap/config/bk7258_ap/usr_gpio_cfg.h`
- `cp/config/bk7258/usr_gpio_cfg.h`

## 3. Project Structure

```text
doorbell_asr/
├── ap/
│   ├── ap_main.c
│   ├── src/                         # Doorbell, provisioning, network, A/V, and ASR
│   ├── include/
│   └── config/bk7258_ap/
├── cp/
│   ├── cp_main.c
│   ├── customer_msg/
│   ├── doorbell_service/
│   └── config/bk7258/
├── partitions/bk7258/
├── app.rst
├── CMakeLists.txt
└── Makefile
```

## 4. Build and Flash

Run from the SDK root:

```bash
make bk7258 PROJECT=doorbell_asr
```

Docker build:

```bash
./dbuild.sh make bk7258 PROJECT=doorbell_asr
```

The generated image is:

```text
build/bk7258/doorbell_asr/package/all-app.bin
```

Flash `all-app.bin` with the BK7258 download tool. After reset, use CP UART0 at
115200 baud to view both CP and forwarded AP logs.

## 5. Startup Flow

1. The CP starts Wi-Fi, the BLE Controller, and the AP.
2. The AP initializes the media service, BLE provisioning, and doorbell application.
3. The AP calls `doorbell_asr_turn_on()` to start ASR automatically.
4. After BLE provisioning, the client can request a UDP, TCP, or CS2 service.

This project does not register a dedicated serial CLI. Doorbell functions are
controlled mainly through the BLE provisioning and network command protocols.

## 6. ASR

The default configuration enables:

- `CONFIG_ASR_SERVICE_WITH_MIC=y`
- `CONFIG_WANSON_ASR=y`
- `CONFIG_WANSON_NEW_LIB_SEG=y`

ASR uses a UAC microphone at 8 kHz with AEC disabled, then resamples the input
to the 16 kHz rate used by the recognition engine. A successful recognition
prints a message similar to:

```text
asr_result : 按门铃
```

The available phrases are defined by the Wanson vocabulary:

```text
ap/components/bk_thirdparty/asr/wanson/bk7258/fst_words.h
```

ASR is stopped when a remote client opens the camera or audio path. It is
started again when that function is closed or the remote client disconnects,
preventing audio-resource conflicts.

## 7. BLE Provisioning and Services

The BLE advertising name has the form `doorbell_XXXXXX`. Provisioning operations
support:

- setting the Wi-Fi STA SSID and password;
- starting the LAN UDP service;
- starting the LAN TCP service;
- setting the CS2 DID, License, Key, and Init String;
- starting the CS2 P2P service;
- disabling BLE.

SoftAP provisioning is not enabled; use BLE provisioning.

Only one doorbell transport service runs at a time. LAN services use these ports:

| Purpose | Port |
| --- | ---: |
| SDP local | 10000 |
| SDP remote | 52110 |
| Command channel | 7100 |
| UDP image | 7180 |
| UDP audio | 7170 |
| TCP image | 7150 |
| TCP audio | 7140 |

The CS2 service requires valid CS2 credentials and a compatible client. Accounts
and credentials are not included in this project.

## 8. Basic Tests

### 8.1 ASR

1. Flash and start the project.
2. Confirm that CP UART0 shows AP startup and ASR initialization logs.
3. Speak a phrase from the Wanson vocabulary into the UAC microphone.
4. Confirm that an `asr_result` message is printed.

### 8.2 BLE Provisioning and LAN Service

1. Connect to `doorbell_XXXXXX` with a client that supports this provisioning protocol.
2. Send the Wi-Fi SSID and password and wait for the device to obtain an IP address.
3. Start either the UDP or TCP service.
4. Connect to command port 7100 and test doorbell control and audio transport.
5. Verify that ASR stops while remote audio is active and restarts afterward.

## 9. Key Configuration

The main settings are in `ap/config/bk7258_ap/config` and
`cp/config/bk7258/config`:

- `CONFIG_INTEGRATION_DOORBELL`
- `CONFIG_INTEGRATION_DOORBELL_CS2`
- `CONFIG_ASR_SERVICE`
- `CONFIG_ASR_SERVICE_WITH_MIC`
- `CONFIG_WANSON_ASR`
- `CONFIG_VOICE_SERVICE`
- `CONFIG_CS2_P2P_SERVER`
- `CONFIG_BLUETOOTH_HOST_ONLY`
- `CONFIG_BTDM_CONTROLLER_ONLY`

## 10. Known Limitations

- The main camera open/close implementation is currently disabled by conditional
  compilation and requires further integration.
- SoftAP provisioning is not enabled.
- ASR defaults to a UAC microphone at 8 kHz; using the onboard microphone
  requires project configuration or code changes.
- The call that opens the camera automatically after ASR recognition is commented out.
- CS2 credentials, the companion mobile application, and a LAN test client are
  not included.
