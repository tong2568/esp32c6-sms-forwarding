#include "wifi_manager.h"
#include "ble/blufi.h"
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "common/task_wdt.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include "config/config.h"
#include "../logger/logger.h"

static WiFiMode s_mode = WIFI_MODE_UNINITIALIZED;

static bool                  s_everConnected = false;
static bool                  s_initDone      = false;  // 初始化完成标志：STA 获取到 IP 或已进入 AP 模式
static WifiReconnectCallback s_reconnectCb   = nullptr;

// 轮询重连状态机
enum ReconnState {
  RECONNECT_IDLE,
  RECONNECT_WAITING,
  RECONNECT_CONNECTING,
  RECONNECT_AP_SCAN_WAIT  // 等待异步扫描完成（AP 模式后台扫描专用）
};

static ReconnState   s_reconnState    = RECONNECT_IDLE;
static int           s_reconnWIdx     = 0;
static int           s_reconnAttempt  = 0;
static unsigned long s_lastAttemptMs  = 0;
static unsigned long s_apRescanNextMs = 0;     // 下次 AP 后台扫描触发时间戳
static bool          s_reconnFromAP   = false;  // 本次重连是否来自 AP 恢复

// ---------- WPA2 强制 + 断开原因日志 ----------

// 常见断开原因码 → 可读描述（wifi_err_reason_t）
static const char* wifiDisconnectReason(uint8_t reason) {
  switch (reason) {
    case 15:  return "4-way握手超时（密码错或加密不匹配）";
    case 200: return "信标超时";
    case 201: return "找不到AP（可能5GHz-only/信号弱/SSID不存在）";
    case 202: return "认证失败";
    case 203: return "关联失败";
    case 204: return "握手超时";
    case 205: return "连接失败";
    case 211: return "认证模式不匹配（WPA3混合模式回退问题）";
    default:  return "未知原因";
  }
}

// STA 断开事件：把 reason 码打印到串口，方便排查连不上网的问题
static void onWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)data;
    LOG("WIFI", "STA断开: reason=%u（%s）", d->reason, wifiDisconnectReason(d->reason));
  }
}

static bool s_wifiEventRegistered = false;
static void ensureWifiEventLogging() {
  if (s_wifiEventRegistered) return;
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onWifiEvent, nullptr);
  s_wifiEventRegistered = true;
}

// 强制用 WPA2 连接：
// arduino-esp32 3.x / ESP-IDF 5.5 的驱动编译了 WPA3-SAE(H2E)，连 WPA2/WPA3 混合 AP
// 时可能协商到 WPA3 而失败（Espressif 已知问题）。这里：
//   1) 认证门槛设为 WPA2；
//   2) 关闭 PMF —— WPA3-SAE 强制要求 PMF，关闭后 STA 无法协商 WPA3，只能回退 WPA2。
// 路由器对手机等其它设备仍可保留 WPA3。
static void beginWpa2(const char* ssid, const char* pass) {
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  WiFi.begin(ssid, pass, 0, nullptr, false);  // 先建配置（含 DHCP），暂不连接

  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    conf.sta.pmf_cfg.capable  = false;   // 不声明 PMF → 无法协商 WPA3
    conf.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
  }
  esp_wifi_connect();
}

// 利用已完成的 WiFi 扫描结果，将 config.wifiList 下标重排到 outOrder[]
// - 可见 SSID 的下标按信号强度（RSSI 降序）排列在前
// - 不可见 SSID 的下标按原始下标顺序追加在后
// 返回值：可见（匹配）的 SSID 数量
// 前提：调用前 WiFi.scanComplete() >= 0
static int buildSortedWifiOrder(int* outOrder, int count) {
  int scanCount = WiFi.scanComplete();
  if (scanCount < 0) scanCount = 0;

  // 为每条配置 SSID 找到最强信号（处理同名 SSID 多 AP 情况）
  // bestRssi[i] = 配置项 i 对应的最强 RSSI；INT_MIN 表示不可见
  int bestRssi[MAX_WIFI_ENTRIES];
  for (int i = 0; i < count; i++) bestRssi[i] = INT_MIN;

  for (int s = 0; s < scanCount; s++) {
    String scannedSsid = WiFi.SSID(s);
    int    rssi        = WiFi.RSSI(s);
    for (int i = 0; i < count; i++) {
      if (config.wifiList[i].ssid == scannedSsid) {
        if (rssi > bestRssi[i]) bestRssi[i] = rssi;
      }
    }
  }

  // 分离可见/不可见列表
  // visible[]: 可见项的 {configIdx, rssi}，用于排序
  struct VisEntry { int idx; int rssi; };
  VisEntry visible[MAX_WIFI_ENTRIES];
  int      visCount   = 0;
  int      invisible[MAX_WIFI_ENTRIES];
  int      invisCount = 0;

  for (int i = 0; i < count; i++) {
    if (bestRssi[i] != INT_MIN) {
      visible[visCount++] = {i, bestRssi[i]};
    } else {
      invisible[invisCount++] = i;
    }
  }

  // 可见列表按 RSSI 降序排序（简单插入排序，MAX_WIFI_ENTRIES=5，开销可忽略）
  for (int a = 1; a < visCount; a++) {
    VisEntry key = visible[a];
    int b = a - 1;
    while (b >= 0 && visible[b].rssi < key.rssi) {
      visible[b + 1] = visible[b];
      b--;
    }
    visible[b + 1] = key;
  }

  // 填充 outOrder[]
  int pos = 0;
  for (int i = 0; i < visCount;   i++) outOrder[pos++] = visible[i].idx;
  for (int i = 0; i < invisCount; i++) outOrder[pos++] = invisible[i];

  return visCount;
}

static void setupSTAMode() {
  WiFi.setHostname(WifiManager::deviceName().c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);  // 认证门槛 WPA2（配合 beginWpa2 的 PMF 关闭强制 WPA2）
}

static void enterAPMode() {
  WiFi.softAP(kApSsid);
  // 原代码为了 BluFi(BLE) 共存开启 Modem Sleep，让 WiFi 让出射频给 BLE 广播。
  // 当前工具链（arduino-esp32 3.x）已无 Bluedroid、BluFi 被禁用，无需再让射频；
  // 而 AP 模式下 Modem Sleep 会让射频在无连接时休眠，客户端发起的 probe/关联请求
  // 可能被漏掉，表现为「能搜到热点但连不上」。这里改为关闭省电，让 AP 保持常醒。
  esp_wifi_set_ps(WIFI_PS_NONE);
  s_mode           = WIFI_MODE_AP_ACTIVE;
  s_initDone       = true;
  s_reconnState    = RECONNECT_IDLE;       // 清除可能残留的扫描等待状态
  s_apRescanNextMs = millis() + WIFI_AP_RESCAN_INTERVAL_MS;  // 30s 后首次后台扫描
  LOG("WIFI", "AP模式启动，SSID: %s，IP: 192.168.4.1", kApSsid);
  Blufi::init();
}

void WifiManager::init() {
  // 重置重连状态机（支持重复调用）
  s_reconnState    = RECONNECT_IDLE;
  s_reconnWIdx     = 0;
  s_reconnAttempt  = 0;
  s_lastAttemptMs  = 0;
  s_apRescanNextMs = 0;
  s_reconnFromAP   = false;
  s_everConnected  = false;
  s_initDone       = false;

  ensureWifiEventLogging();  // 注册断开原因日志（首次生效）

  // 软重启（ESP.restart）后 WiFi 驱动状态可能残留，强制关闭后再初始化
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);
  taskWdtReset();

  if (config.wifiCount == 0) {
    LOG("WIFI", "未配置任何WiFi，直接进入AP模式");
    enterAPMode();
    return;
  }

  // 设置客户端主机名（路由器 DHCP 列表中显示的名称），格式：SMS-Forwarder-XXXXXXXX
  setupSTAMode();

  // 扫描当前环境 WiFi，优先连接可见 SSID
  WiFi.scanNetworks(false);  // 阻塞扫描，此处处于 setup() 或 AP 恢复前，WDT 已有保护
  taskWdtReset();
  int initOrder[MAX_WIFI_ENTRIES] = {};
  int initMatchCount = buildSortedWifiOrder(initOrder, config.wifiCount);
  WiFi.scanDelete();  // 释放阻塞扫描结果内存（与 AP tick 路径保持一致）
  LOG("WIFI", "扫描完成，%d/%d 条配置SSID当前可见，将优先连接", initMatchCount, config.wifiCount);

  for (int oi = 0; oi < config.wifiCount; oi++) {
    int w = initOrder[oi];
    if (config.wifiList[w].ssid.length() == 0) continue;

    const char* ssid = config.wifiList[w].ssid.c_str();
    const char* pass = config.wifiList[w].password.c_str();
    LOG("WIFI", "尝试第 %d/%d 条WiFi: %s", w + 1, config.wifiCount, ssid);

    for (int attempt = 1; attempt <= WIFI_RECONNECT_ATTEMPTS_PER_SSID; attempt++) {
      beginWpa2(ssid, pass);
      unsigned long start = millis();
      while (millis() - start < 10000) {
        if (WiFi.status() == WL_CONNECTED) {
          s_mode          = WIFI_MODE_STA_CONNECTED;
          s_everConnected = true;
          s_initDone      = true;
          WiFi.setSleep(false);  // 关闭 WiFi Modem Sleep，避免 TCP SYN 丢包导致 3 秒连接延迟
          LOG("WIFI", "第 %d/%d 条WiFi，第 %d/%d 次连接成功，IP: %s", w + 1, config.wifiCount, attempt, WIFI_RECONNECT_ATTEMPTS_PER_SSID, WiFi.localIP().toString().c_str());
          return;
        }
        delay(100);
        taskWdtReset();
      }

      LOG("WIFI", "第 %d/%d 条WiFi，第 %d/%d 次连接超时", w + 1, config.wifiCount, attempt, WIFI_RECONNECT_ATTEMPTS_PER_SSID);
      // disconnect(false)：只断开 AP 连接，保持 STA 模式开启
      // 避免 disconnect(true) 关掉 STA 后 begin() 的 enableSTA(true) 触发
      // ESP_ERR_WIFI_STATE（0x3014）"STA enable failed!" 的驱动状态竞争
      WiFi.disconnect(false);
      delay(500);
      taskWdtReset();
    }
  }

  LOG("WIFI", "所有WiFi条目全部失败，切换到AP模式");
  enterAPMode();
}

void WifiManager::tick() {
  if (s_mode == WIFI_MODE_AP_ACTIVE) {
    if (config.wifiCount == 0) return;  // 无配置，无需扫描

    if (s_reconnState == RECONNECT_AP_SCAN_WAIT) {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) return;  // 扫描进行中，下次 tick 继续轮询
      if (n < 0) {
        // 扫描失败（WIFI_SCAN_FAILED），重新调度，避免状态机永久卡在 SCAN_WAIT
        LOG("WIFI", "AP模式后台扫描失败，%lums后重试", WIFI_AP_RESCAN_INTERVAL_MS);
        s_reconnState    = RECONNECT_IDLE;
        s_apRescanNextMs = millis() + WIFI_AP_RESCAN_INTERVAL_MS;
        return;
      }

      int apOrder[MAX_WIFI_ENTRIES] = {};
      int matchCount = buildSortedWifiOrder(apOrder, config.wifiCount);
      // apOrder[] 在此路径中仅用于 matchCount>0 判断；重连顺序由 RECONNECT_WAITING 状态机
      // 从 config.wifiList[0] 顺序遍历（与 init 扫描重排路径不同，属于已知差异）
      WiFi.scanDelete();  // 释放扫描结果内存，避免内存泄漏

      if (matchCount > 0) {
        LOG("WIFI", "AP模式后台扫描发现 %d 条匹配SSID，切换到重连状态机", matchCount);
        // 关闭 AP，切到 STA 模式，交由非阻塞重连状态机处理
        WiFi.softAPdisconnect(true);
        delay(100);
        taskWdtReset();
        setupSTAMode();
        s_mode          = WIFI_MODE_RECONNECTING;
        s_reconnFromAP  = true;
        s_reconnState   = RECONNECT_WAITING;
        s_reconnWIdx    = 0;
        s_reconnAttempt = 0;
        s_lastAttemptMs = 0;  // 使 WAITING 立即触发首次尝试
      } else {
        LOG("WIFI", "AP模式后台扫描无匹配SSID，%lums后重试", WIFI_AP_RESCAN_INTERVAL_MS);
        s_reconnState    = RECONNECT_IDLE;
        s_apRescanNextMs = millis() + WIFI_AP_RESCAN_INTERVAL_MS;
      }
      return;
    }

    // 定时触发异步扫描
    if (millis() >= s_apRescanNextMs) {
      LOG("WIFI", "AP模式启动后台WiFi扫描...");
      WiFi.scanNetworks(true);  // 异步，不阻塞 loop()
      s_reconnState = RECONNECT_AP_SCAN_WAIT;
    }
    return;
  }

  switch (s_reconnState) {
    case RECONNECT_IDLE:
      if (s_everConnected && s_mode == WIFI_MODE_STA_CONNECTED && WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        s_mode = WIFI_MODE_RECONNECTING;
        s_reconnState   = RECONNECT_WAITING;
        s_reconnWIdx    = 0;
        s_reconnAttempt = 0;
        s_lastAttemptMs = 0;  // 使 WAITING 立即触发首次尝试
        LOG("WIFI", "检测到WiFi断线，启动重连轮询");
      }
      break;

    case RECONNECT_WAITING:
      if (millis() - s_lastAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
        // 跳过空 SSID
        while (s_reconnWIdx < config.wifiCount && config.wifiList[s_reconnWIdx].ssid.length() == 0) {
          s_reconnWIdx++;
        }
        if (s_reconnWIdx >= config.wifiCount) {
          // 所有 SSID 已尝试完一轮
          if (s_reconnFromAP) {
            // 来自 AP 恢复的重连，全部失败则退回 AP 模式，等待下一轮后台扫描
            LOG("WIFI", "AP恢复重连全部失败，重新进入AP模式等待下次扫描");
            s_reconnFromAP = false;
            enterAPMode();
            return;
          }
          // 普通 STA 断线重连，重置后继续无限循环
          s_reconnWIdx    = 0;
          s_reconnAttempt = 0;
          LOG("WIFI", "所有 SSID 重连均失败，重置循环重试");
        }
        const char* ssid = config.wifiList[s_reconnWIdx].ssid.c_str();
        const char* pass = config.wifiList[s_reconnWIdx].password.c_str();
        LOG("WIFI", "重连尝试 SSID: %s，第 %d/%d 次", ssid, s_reconnAttempt + 1, WIFI_RECONNECT_ATTEMPTS_PER_SSID);
        WiFi.disconnect(true);
        delay(300);
        beginWpa2(ssid, pass);
        s_lastAttemptMs = millis();
        s_reconnState   = RECONNECT_CONNECTING;
      }
      break;

    case RECONNECT_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        s_mode          = WIFI_MODE_STA_CONNECTED;
        s_reconnState   = RECONNECT_IDLE;
        s_reconnFromAP  = false;  // AP 恢复成功后清除标志，防止后续普通断线走 AP 回退路径
        WiFi.setSleep(false);  // 重连后重新关闭 Modem Sleep
        LOG("WIFI", "重连成功，SSID: %s，IP: %s", config.wifiList[s_reconnWIdx].ssid.c_str(), WiFi.localIP().toString().c_str());
        if (s_reconnectCb) s_reconnectCb();
      } else if (millis() - s_lastAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
        LOG("WIFI", "重连超时，SSID: %s，第 %d/%d 次", config.wifiList[s_reconnWIdx].ssid.c_str(), s_reconnAttempt + 1, WIFI_RECONNECT_ATTEMPTS_PER_SSID);
        WiFi.disconnect(true);
        s_reconnAttempt++;
        if (s_reconnAttempt >= WIFI_RECONNECT_ATTEMPTS_PER_SSID) {
          s_reconnAttempt = 0;
          s_reconnWIdx++;
        }
        s_lastAttemptMs = millis();
        s_reconnState   = RECONNECT_WAITING;
      }
      break;
  }
}

WiFiMode WifiManager::mode() {
  return s_mode;
}

bool WifiManager::isInitDone() {
  return s_initDone;
}

String WifiManager::ip() {
  if (s_mode == WIFI_MODE_STA_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return IPAddress(192, 168, 4, 1).toString();
}

String WifiManager::deviceUrl() {
  return "http://" + ip() + "/";
}

String WifiManager::deviceId() {
  static String s_id = "";
  if (s_id.length() > 0) {
    return s_id;
  }
  uint64_t mac = ESP.getEfuseMac();
  uint32_t last3 = (uint32_t)(((mac >> 24) & 0xFF) << 16 |
                              ((mac >> 32) & 0xFF) << 8  |
                              ((mac >> 40) & 0xFF));
  s_id = String(last3, HEX);
  s_id.toUpperCase();
  while (s_id.length() < 6) {
    s_id = "0" + s_id;
  }
  return s_id;
}

String WifiManager::deviceName() {
  return String(APP_NAME "-") + deviceId();
}

void WifiManager::setReconnectCallback(WifiReconnectCallback cb) {
  s_reconnectCb = cb;
}
