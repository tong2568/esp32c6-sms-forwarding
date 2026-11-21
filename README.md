# 低成本短信转发器（ESP32-C6 SuperMini 移植版）

> 本仓库是 [maxming2333/esp32-sms-forwarding](https://github.com/maxming2333/esp32-sms-forwarding) 的 **ESP32-C6 SuperMini 移植 fork**；上游又来自 [chenxuuu/sms_forwarding](https://github.com/chenxuuu/sms_forwarding)，在此基础上改用 C++17 + PlatformIO 重构。

基于 **ESP32-C6 SuperMini（4MB Flash）** + ML307R/C/A 的低成本短信转发器，支持多种推送方式，适合需要远程接收短信通知的场景，如验证码接收、物联网设备监控等。

> 编译：`pio run -e esp32c6_supermini`；烧录：`esptool --chip esp32c6 ...`（详见下文「ESP32-C6 SuperMini 移植说明」）。本仓库的 Release 只发布 C6 固件。

## 与上游的主要差异

- **主控换成 ESP32-C6 SuperMini（4MB）**：接线改为 `EN=GPIO19`、`TX=GPIO20`、`RX=GPIO14`（见下方接线图）。
- **在线升级指向本仓库**：CI 构建时自动注入当前仓库地址，设备只升级本仓库发布的 C6 固件；C3 构建关闭在线升级。
- **WiFi 强制 WPA2**：规避 ESP-IDF 5.5 在 WPA2/WPA3 混合模式下的回连问题（路由器其它设备仍可保留 WPA3）。
- **默认关闭蜂窝数据**：初始化即 `AT+CGACT=0,1`，避免无流量套餐的卡产生数据扣费；通知走 WiFi。
- **BluFi（BLE 配网）禁用**：arduino-esp32 3.x 已改用 NimBLE、无 Bluedroid；AP 网页配网（`SMS-Forwarder-AP` / `192.168.4.1`）保留。
- **日志增加 64 行内存环形缓冲**：`/api/logs` 在未开启「写入日志文件」时也能查看最近日志。
- 修复 ESP-IDF 5.x / mbedTLS 3.x 编译兼容性、`task_wdt` 刷屏问题。

> 视频教程：[B站视频](https://www.bilibili.com/video/BV1cSmABYEiX)

<img src="assets/photo.png" width="200" />

## 功能

- 支持使用通用 AT 指令与模块通信
- 开机后通过 Web 界面配置短信转发参数、查询当前状态
- 支持最多 10 个推送通道同时启用，每个通道可独立配置
- 推送策略：广播（全部执行）/ 故障转移（逐个尝试直至成功）
- 支持为每个推送通道配置自定义消息格式（含 9 种占位符）
- 支持最多 5 个 WiFi 凭据，按序连接，全部失败时自动开启 AP 模式
- 通过 Web 界面配置 WiFi，无需修改代码重烧固件
- 长短信自动合并（30 秒超时）
- 支持来电通知功能
- 支持号码黑名单（短信和来电均拦截）
- 定时重启（每日定时或按间隔），防止长期运行异常
- 配置导入导出（语义化嵌套 JSON 文件，浏览器直接下载）
- 配置一键重置为出厂默认值（CSRF token 保护）
- SIM 卡热插拔检测（运行中插入自动初始化）
- 设备状态监控（信号强度、网络状态、SIM 状态、Flash 用量）
- 通过 Web 界面主动发送短信（消耗余额保号）
- 通过 Web 界面发起 Ping 测试（极低流量消耗余额）
- 管理员短信远程控制（发送短信、重启设备）
- 自定义固件应用描述信息（ESPConnect 可显示真实版本）

## 推送通道支持

支持以下 12 种推送方式，最多可同时启用 10 个通道，每个通道可独立配置推送策略。

| # | 推送方式 | 说明 | 所需配置 |
|---|---------|------|---------|
| 1 | POST JSON | 通用 HTTP POST（JSON 格式） | URL |
| 2 | Bark | iOS/macOS 推送服务 | Bark 服务端 URL |
| 3 | GET 请求 | URL 参数方式（自动 URL 编码） | URL（含 `{sender}` 等占位符） |
| 4 | 钉钉机器人 | 企业群通知 | Webhook URL；可选：Secret 加签密钥 |
| 5 | PushPlus | 微信公众号推送 | Token |
| 6 | Server酱 | 微信推送服务 | SendKey |
| 7 | POST 请求 | 完全自定义 URL + 消息格式 | URL + 自定义消息格式 |
| 8 | 飞书机器人 | 企业群通知 | Webhook URL；可选：Secret 加签密钥 |
| 9 | Gotify | 自托管推送服务 | Gotify 服务地址 + App Token |
| 10 | Telegram 机器人 | Telegram Bot API | Bot Token + Chat ID |
| 11 | 企业微信机器人 | 企业微信群通知 | Webhook URL；可选：Secret 加签密钥 |
| 12 | SMS 备份推送 | 网络不可用时通过 SIM 卡发短信 | 目标手机号 |

### 推送策略

- **广播**：所有已启用通道依次执行，无论前一个是否成功
- **故障转移**：按顺序逐个尝试，某个通道成功后停止，不再尝试后续通道

### 自定义消息格式

每个推送通道可单独设置消息格式，留空则使用该通道的内置默认格式。支持以下占位符：

| 占位符 | 说明 |
|--------|------|
| `{sender}` | 短信发送方号码 |
| `{message}` | 短信内容 |
| `{timestamp}` | Unix 时间戳（秒） |
| `{date}` | 当前时间（YYYY-MM-DD HH:mm:ss） |
| `{device_id}` | 设备唯一 ID |
| `{carrier}` | 运营商名称 |
| `{sim_number}` | 本机 SIM 卡号码 |
| `{sim_slot}` | 卡槽标识 |
| `{signal}` | 信号强度 |

|状态信息|主动 Ping|
|-|-|
|![](assets/status.png)|![](assets/ping.png)|

## 硬件搭配

如果希望自行焊接硬件，参考下面的硬件搭配，总成本约¥27.8，仅支持移动/联通卡。

- ESP32-C3开发板，当前选用[ESP32-C3 Super Mini](https://item.taobao.com/item.htm?id=852057780489&skuId=5813710390565)，¥9.5包邮
- ML307R/C/A开发板，当前选用[小蓝鲸ML307R/C/A核心板](https://item.taobao.com/item.htm?id=797466121802&skuId=5722077108045)，¥16.3包邮
- [4G FPC天线](https://item.taobao.com/item.htm?id=797466121802&skuId=5722077108045)，¥2，与核心板同购

若希望直接使用成品，可选直接购以下套件，支持移动/联通/电信卡：

- [小蓝鲸WIFI短信宝](https://item.taobao.com/item.htm?id=1003711355912)
- [4G FPC天线](https://item.taobao.com/item.htm?id=1003711355912&skuId=6162872574943)，与开发板同购

## 硬件连接

ESP32-C3 与 ML307R/C/A 通过串口（UART）连接，接线如下：

```
┌──────────────────────────────────────────────┐
|                                              |
|   ESP32-C3 Super Mini     ML307R/C/A核心板    |
| ┌───────────────────┐    ┌─────────────────┐ |
└─┼─ GPIO5 (MODEM_EN) │    │                 │ |
  │       GPIO3 (TX) ─┼───►│ RX              │ |
  │                   │    │             EN ─┼─┘
  │       GPIO4 (RX) ◄┼────┤ TX              │ 
  │                   │    │                 │ 
  │              GND ─┼────┤ GND             │ 
  │                   │    │                 │ 
  │               5V ─┼────┤ VCC (5V)        |
  │                   │    │                 │
  └───────────────────┘    └─────────────────┘
                           │                 │
                           │  SIM卡槽         │
                           │  (插入Nano SIM)  │
                           │                 │
                           │  天线接口        │
                           │  (连接4G天线)    │
                           └─────────────────┘
```

模块 EN 引脚与 ESP32-C3 的 GPIO5 连接，可通过软件控制模块上下电。可通过 USB 连接 ESP32-C3 进行编程和供电，正常工作时虚拟串口数据将直接转发至 ML307R/C/A，方便调试。

## ESP32-C6 SuperMini 移植说明

若你使用的是 **ESP32-C6 SuperMini（4MB Flash）**，可直接使用本仓库（无需改代码），接线与 C3 不同，见下表。

### 接线（ESP32-C6 SuperMini ↔ ML307R/C/A）

按 ML307 模块与 C6 SuperMini 的插针位置接线：模块 EN 直插 GPIO19，RX/TX 飞线到 GPIO20/14。

```
┌──────────────────────────────────────────────┐
|                                              |
|  ESP32-C6 SuperMini       ML307R/C/A核心板    |
| ┌───────────────────┐    ┌─────────────────┐ |
└─┼─ GPIO19 (MODEM_EN)│    │                 │ |
  │        GPIO20 (TX) ─┼──►│ RX              │ |
  │                   │    │             EN ─┼─┘
  │        GPIO14 (RX) ◄┼────┤ TX              │
  │                   │    │                 │
  │              GND ─┼────┤ GND             │
  │                   │    │                 │
  │               5V ─┼────┤ VCC (5V)        │
  │                   │    │                 │
  └───────────────────┘    └─────────────────┘
                           │  SIM卡槽 + 天线  │
                           └─────────────────┘
```

| 信号 | ESP32-C6 SuperMini | ML307R/C/A |
|------|--------------------|------------|
| 模组使能 EN | GPIO19 | EN |
| 串口 TX（发） | GPIO20 | RX |
| 串口 RX（收） | GPIO14 | TX |
| 地 | GND | GND |
| 电源 | 5V | VCC |

> 如需改引脚，编辑 [`src/main.cpp`](src/main.cpp) 顶部的 `TXD/RXD/MODEM_EN_PIN` 宏即可（GPIO20/14 均为 C6 普通 GPIO，无 strapping/JTAG/Flash 复用，可直接用于 UART）。

### 编译与烧录（C6）

```bash
# 编译（default_envs 已指向 C6，直接 pio run 即可）
pio run
# 或显式指定环境
pio run -e esp32c6_supermini

# 烧录固件 + Web UI 文件系统
pio run -e esp32c6_supermini -t upload
pio run -e esp32c6_supermini -t uploadfs
```

命令行全量烧录（esptool）：

```bash
pip install "esptool>=4.8"
esptool --chip esp32c6 --baud 460800 write_flash 0x0 full.bin
```

> 注意 C6 的 `--chip` 参数是 `esp32c6`（C3 才是 `esp32c3`）。

## Release 与在线升级（仅 C6）

- 推送到 `master` 后，GitHub Actions 会自动构建 C6 固件并发布 Release（`full-*.bin`、`update-*.bin`、`ota-*.bin`、`littlefs-*.bin`、`firmware-*.elf`）。
- 固件内的在线升级地址由 CI 注入为当前仓库，例如 `https://github.com/<owner>/<repo>/releases`。
- 本地构建想指向自己的 Release，可用环境变量覆盖：

  ```bash
  OTA_GITHUB_REPO=<owner>/<repo> pio run -e esp32c6_supermini
  ```

- ESP32-C3 构建关闭在线升级（本 fork 只发布 C6 固件），C3 请使用上游仓库的 Release。

## 快速开始

### 方式一：图形化烧录（推荐，零安装）

1. 从 [GitHub Releases](../../releases/latest) 下载最新版固件压缩包，解压获得 `full.bin`
2. 使用 Chrome 或 Edge 浏览器（88+）打开 [ESPConnect](https://thelastoutpostworkshop.github.io/ESPConnect/)
3. 波特率选择 `460800`
4. 点击「连接」，在浏览器弹窗中选择 "USB JTAG/serial debug unit" 设备
5. 进入「闪存工具」→「烧录固件」，上传 `full.bin` 即可完成全量烧录

### 方式二：命令行烧录（进阶）

```bash
pip install "esptool>=4.8"

# 全量烧录（推荐，地址 0x0）
esptool --chip esp32c6 --baud 460800 write_flash 0x0 full.bin

# 单独更新 Web UI 文件系统（地址 0x3B0000）
esptool --chip esp32c6 --baud 460800 write_flash 0x3B0000 littlefs.bin
```

### 固件文件说明

| 文件 | 用途 | 烧录地址 |
|------|------|---------|
| `full.bin` | 全量固件（bootloader + partitions + firmware + Web UI），首次烧录推荐 | `0x0` |
| `main.bin` | 主固件（bootloader + partitions + firmware），不含 Web UI | `0x0` |
| `littlefs.bin` | 仅 Web UI 文件系统，更新页面时单独使用 | `0x3B0000` |

## 首次配置

1. 设备上电后，若未配置 WiFi，自动进入 AP 模式（热点名：`SMS-Forwarder-AP`，无密码）
2. 连接该热点，浏览器访问 `http://192.168.4.1` 打开配置界面
3. 在 WiFi 配置区域填入路由器的 SSID 和密码，保存后设备自动重启
4. 设备重启后连接路由器，通过路由器分配的 IP 访问管理界面
5. 默认 Web 界面账号/密码为 `admin / admin123`，请尽快修改
6. 在推送通道区域添加并配置推送方式，保存配置


## 引脚定义

### Maker Go ESP32-C3 Supermini

- [makergo_esp32c3_supermini.json](boards/makergo_esp32c3_supermini.json)
- [pins_arduino.h](custom_variants/super_mini_esp32c3/pins_arduino.h)

### ESP32-C6 SuperMini（本仓库新增）

- [esp32c6_supermini.json](boards/esp32c6_supermini.json)
- [pins_arduino.h](custom_variants/super_mini_esp32c6/pins_arduino.h)

## 软件组成

**ESP32-C3（C++17，PlatformIO）**：

- `src/config/` — 配置结构体，NVS（非易失存储）读写，字段校验
- `src/push/` — 推送通道分发，12 种推送类型实现，消息模板渲染
- `src/email/` — SMTP 邮件发送（ReadyMail）
- `src/sms/` — UART 读取，PDU 格式解析（pdulib），长短信重组
- `src/http/` — HTTP 路由（ESPAsyncWebServer），Basic Auth，API 控制器
- `src/sim/` — SIM 卡 AT 指令，状态机，热插拔，运营商/号码查询
- `src/wifi/` — 多 WiFi 有序连接，AP 模式管理
- `src/time/` — 时间同步（SIM NITZ + NTP）
- `data/index.html` — 配置管理页面（LittleFS）
- `data/tools.html` — 工具箱页面（LittleFS）

**ML307R/C/A**：运行出厂 AT 固件，无需改动。

**主要依赖库**（`platformio.ini` 自动管理）：

| 库 | 用途 |
|----|------|
| `pdulib@^0.5.11` | PDU 格式短信解析 |
| `ReadyMail@^0.3.8` | SMTP 邮件发送 |
| `ESPAsyncWebServer` | 异步 HTTP Web 服务 |
| `ArduinoJson@^7.4.0` | JSON 序列化/反序列化 |


## 本地构建

**安装 PlatformIO**：通过 [VS Code 插件](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide) 或命令行安装：

```bash
pip install platformio
```

**克隆仓库**：

```bash
git clone https://github.com/maxming2333/esp32-sms-forwarding.git
cd esp32-sms-forwarding
```

**编译固件**：

```bash
pio run
```

**烧录固件**：

```bash
pio run -t upload
```

**构建并上传 LittleFS（Web UI 文件系统）**：

```bash
pio run -t uploadfs
```

> `uploadfs` 会先通过 Python 脚本 gzip 压缩 `data/` 目录下的 HTML 文件，再上传至 ESP32 的 LittleFS 分区（地址 `0x3B0000`）。
