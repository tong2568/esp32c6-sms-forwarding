#include "esim.h"
#include <string.h>
#include <vector>
#include "sim/sim_dispatcher.h"

// ---------------------------------------------------------------------------
// 参考实现：SGP.22 ES10c（GetEID / GetProfilesInfo / EnableProfile）
// APDU 通道：AT+CCHO 选择 ISD-R AID -> AT+CGLA 收发 STORE DATA -> AT+CCHC 关闭
// ---------------------------------------------------------------------------

namespace {

constexpr const char* ISDR_AID = "A0000005591010FFFFFFFF8900000100";
constexpr size_t STORE_DATA_MSS = 120;
constexpr int    GET_RESPONSE_CHAIN_MAX = 64;

struct Tlv {
  std::vector<uint8_t> tag;
  std::vector<uint8_t> value;
  std::vector<Tlv>     children;
  bool                 constructed = false;
};

bool tagIs(const Tlv& t, std::initializer_list<uint8_t> tag) {
  if (t.tag.size() != tag.size()) return false;
  size_t i = 0;
  for (uint8_t b : tag) {
    if (t.tag[i++] != b) return false;
  }
  return true;
}

const Tlv* firstChild(const Tlv& t, std::initializer_list<uint8_t> tag) {
  for (const Tlv& c : t.children) {
    if (tagIs(c, tag)) return &c;
  }
  return nullptr;
}

bool parseTlvAt(const std::vector<uint8_t>& d, size_t end, size_t& pos, Tlv& out, String& msg) {
  if (pos >= end) {
    msg = "TLV 数据为空";
    return false;
  }
  uint8_t b = d[pos++];
  out.tag.push_back(b);
  if ((b & 0x1F) == 0x1F) {
    do {
      if (pos >= end) { msg = "TLV tag 越界"; return false; }
      b = d[pos++];
      out.tag.push_back(b);
    } while (b & 0x80);
  }
  if (pos >= end) { msg = "TLV 长度缺失"; return false; }
  size_t len = d[pos++];
  if (len & 0x80) {
    int n = (int)(len & 0x7F);
    if (n < 1 || n > 3 || pos + (size_t)n > end) { msg = "TLV 长度字段非法"; return false; }
    len = 0;
    for (int i = 0; i < n; i++) len = (len << 8) | d[pos++];
  }
  if (pos + len > end) { msg = "TLV 长度越界"; return false; }
  out.value.assign(d.begin() + pos, d.begin() + pos + len);
  out.constructed = (out.tag[0] & 0x20) != 0;
  pos += len;
  if (out.constructed) {
    size_t cpos = 0;
    while (cpos < out.value.size()) {
      Tlv child;
      if (!parseTlvAt(out.value, out.value.size(), cpos, child, msg)) return false;
      out.children.push_back(child);
    }
  }
  return true;
}

bool parseTlv(const std::vector<uint8_t>& d, Tlv& out, String& msg) {
  size_t pos = 0;
  return parseTlvAt(d, d.size(), pos, out, msg);
}

int tlvInt(const Tlv& t, int def = 0) {
  if (t.value.empty()) return def;
  size_t start = t.value.size() > 4 ? t.value.size() - 4 : 0;
  uint32_t v = 0;
  for (size_t i = start; i < t.value.size(); i++) v = (v << 8) | t.value[i];
  return (int)v;
}

String bytesToHex(const std::vector<uint8_t>& v) {
  static const char* kHex = "0123456789ABCDEF";
  String s;
  s.reserve(v.size() * 2);
  for (uint8_t b : v) {
    s += kHex[(b >> 4) & 0x0F];
    s += kHex[b & 0x0F];
  }
  return s;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hexToBytes(const String& s, std::vector<uint8_t>& out) {
  if (s.length() == 0 || (s.length() & 1)) return false;
  out.clear();
  out.reserve(s.length() / 2);
  for (size_t i = 0; i < s.length(); i += 2) {
    int hi = hexNibble(s[i]);
    int lo = hexNibble(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  return true;
}

// ICCID 等 GSM BCD：低半字节在前，0xF 作为填充
String bcdDecode(const std::vector<uint8_t>& v) {
  static const char* kHex = "0123456789ABCDEF";
  String s;
  s.reserve(v.size() * 2);
  for (uint8_t b : v) {
    uint8_t lo = b & 0x0F;
    uint8_t hi = (b >> 4) & 0x0F;
    if (lo != 0x0F) s += kHex[lo];
    if (hi != 0x0F) s += kHex[hi];
  }
  return s;
}

// EID：高半字节在前；异常时回退为 HEX
String eidDecode(const std::vector<uint8_t>& v) {
  String s;
  s.reserve(v.size() * 2);
  for (uint8_t b : v) {
    uint8_t hi = (b >> 4) & 0x0F;
    uint8_t lo = b & 0x0F;
    if (hi > 9 || lo > 9) return bytesToHex(v);
    s += (char)('0' + hi);
    s += (char)('0' + lo);
  }
  return s;
}

void appendTlv(std::vector<uint8_t>& out, std::initializer_list<uint8_t> tag,
               const std::vector<uint8_t>& value) {
  out.insert(out.end(), tag.begin(), tag.end());
  if (value.size() < 0x80) {
    out.push_back((uint8_t)value.size());
  } else if (value.size() < 0x100) {
    out.push_back(0x81);
    out.push_back((uint8_t)value.size());
  } else {
    out.push_back(0x82);
    out.push_back((uint8_t)(value.size() >> 8));
    out.push_back((uint8_t)(value.size() & 0xFF));
  }
  out.insert(out.end(), value.begin(), value.end());
}

String firstLineContaining(const String& resp, const char* needle) {
  int p = resp.indexOf(needle);
  if (p < 0) return "";
  int start = resp.lastIndexOf('\n', p);
  start = (start < 0) ? 0 : start + 1;
  int end = resp.indexOf('\n', p);
  if (end < 0) end = resp.length();
  String line = resp.substring(start, end);
  line.trim();
  return line;
}

String quotedHex(const String& line) {
  int q1 = line.indexOf('"');
  int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
  if (q1 >= 0 && q2 > q1) return line.substring(q1 + 1, q2);
  int comma = line.indexOf(',');
  if (comma < 0) return "";
  String h = line.substring(comma + 1);
  h.trim();
  if (h.startsWith("\"")) h.remove(0, 1);
  if (h.endsWith("\"")) h.remove(h.length() - 1);
  return h;
}

bool parseChannel(const String& resp, int& ch) {
  String line = firstLineContaining(resp, "+CCHO:");
  if (line.length() > 0) {
    int c = line.indexOf(':');
    if (c >= 0) {
      int v = line.substring(c + 1).toInt();
      if (v > 0) { ch = v; return true; }
    }
  }
  // 部分模组只返回裸数字
  int pos = 0;
  while (pos < (int)resp.length()) {
    int end = resp.indexOf('\n', pos);
    if (end < 0) end = resp.length();
    String row = resp.substring(pos, end);
    row.trim();
    if (row.length() > 0 && row != "OK" && !row.startsWith("AT+")) {
      int v = row.toInt();
      if (v > 0) { ch = v; return true; }
    }
    pos = end + 1;
  }
  return false;
}

uint16_t swOf(const std::vector<uint8_t>& r) {
  if (r.size() < 2) return 0;
  return (uint16_t)((r[r.size() - 2] << 8) | r[r.size() - 1]);
}

bool swOk(uint16_t sw) { return sw == 0x9000 || (sw >> 8) == 0x91; }

String swText(uint16_t sw) {
  char buf[48];
  switch (sw) {
    case 0x9000: return "OK";
    case 0x6A82: return "未找到 eUICC 应用或 Profile";
    case 0x6985: return "卡策略拒绝当前操作";
    case 0x6A86: return "APDU 参数不被卡接受";
    case 0x6D00: return "eUICC 不支持该 APDU 指令";
    case 0x6E00: return "APDU 逻辑通道不被接受";
    default:
      snprintf(buf, sizeof(buf), "APDU 状态字 %04X", sw);
      return String(buf);
  }
}

uint8_t channelCla(int ch) {
  if (ch < 4) return (uint8_t)(0x80 | ch);
  return (uint8_t)(0xC0 | (ch - 4));
}

bool atCmd(const String& cmd, unsigned long timeout, String& resp) {
  resp = "";
  SimDispatcher::sendCommand(cmd.c_str(), timeout, &resp, false, SIM_RESP_LARGE_BUF_SIZE);
  return true;
}

bool openChannel(int& ch, String& msg) {
  String resp;
  String cmd = String("AT+CCHO=\"") + ISDR_AID + "\"";
  atCmd(cmd, 8000, resp);
  if (!parseChannel(resp, ch)) {
    // 清理可能残留的逻辑通道后重试一次
    for (int c = 1; c <= 3; c++) {
      String r;
      atCmd(String("AT+CCHC=") + c, 3000, r);
    }
    resp = "";
    atCmd(cmd, 8000, resp);
  }
  if (!parseChannel(resp, ch)) {
    msg = "未检测到 eUICC 应用（当前卡可能不是实体 eSIM，或模组不支持 CCHO/CGLA）";
    return false;
  }
  if (ch <= 0 || ch > 19) {
    atCmd(String("AT+CCHC=") + ch, 3000, resp);
    msg = "模组返回了不支持的逻辑通道";
    return false;
  }
  return true;
}

void closeChannel(int ch) {
  String r;
  atCmd(String("AT+CCHC=") + ch, 3000, r);
}

bool transmit(int ch, const std::vector<uint8_t>& apdu, std::vector<uint8_t>& out, String& msg) {
  String hex = bytesToHex(apdu);
  String cmd = String("AT+CGLA=") + ch + "," + hex.length() + ",\"" + hex + "\"";
  String resp;
  atCmd(cmd, 15000, resp);

  String line = firstLineContaining(resp, "+CGLA:");
  if (line.length() == 0) {
    msg = "模组未返回 APDU 数据";
    return false;
  }
  String h = quotedHex(line);
  if (h.length() == 0) {
    msg = "模组 APDU 响应格式无法解析";
    return false;
  }
  if (!hexToBytes(h, out)) {
    msg = "模组 APDU 响应不是合法 HEX";
    return false;
  }
  if (out.size() < 2) {
    msg = "APDU 响应过短";
    return false;
  }
  return true;
}

bool collect(int ch, const std::vector<uint8_t>& first, std::vector<uint8_t>& out, String& msg) {
  std::vector<uint8_t> resp = first;
  int chain = 0;
  for (;;) {
    uint16_t sw = swOf(resp);
    out.insert(out.end(), resp.begin(), resp.end() - 2);
    if (swOk(sw)) return true;
    if ((sw >> 8) != 0x61) {
      msg = swText(sw);
      return false;
    }
    if (++chain > GET_RESPONSE_CHAIN_MAX) {
      msg = "APDU 分段响应过多";
      return false;
    }
    uint8_t le = (uint8_t)(sw & 0xFF);
    std::vector<uint8_t> getResp = {channelCla(ch), 0xC0, 0x00, 0x00, le};
    std::vector<uint8_t> next;
    if (!transmit(ch, getResp, next, msg)) return false;
    resp = next;
  }
}

bool storeData(int ch, const std::vector<uint8_t>& payload, std::vector<uint8_t>& out, String& msg) {
  out.clear();
  size_t offset = 0;
  uint8_t block = 0;
  while (offset < payload.size()) {
    size_t n = payload.size() - offset;
    if (n > STORE_DATA_MSS) n = STORE_DATA_MSS;
    bool last = (offset + n == payload.size());
    std::vector<uint8_t> apdu;
    apdu.reserve(5 + n);
    apdu.push_back(channelCla(ch));
    apdu.push_back(0xE2);
    apdu.push_back(last ? 0x91 : 0x11);
    apdu.push_back(block);
    apdu.push_back((uint8_t)n);
    apdu.insert(apdu.end(), payload.begin() + offset, payload.begin() + offset + n);

    std::vector<uint8_t> resp;
    if (!transmit(ch, apdu, resp, msg)) return false;
    if (!collect(ch, resp, out, msg)) return false;
    offset += n;
    block++;
  }
  return true;
}

// ES10c GetEID: BF3E 03 5C 01 5A
bool getEid(int ch, String& eid, String& msg) {
  std::vector<uint8_t> req = {0xBF, 0x3E, 0x03, 0x5C, 0x01, 0x5A};
  std::vector<uint8_t> raw;
  if (!storeData(ch, req, raw, msg)) return false;

  Tlv t;
  if (!parseTlv(raw, t, msg)) return false;
  if (!tagIs(t, {0xBF, 0x3E})) { msg = "EID 响应 tag 不匹配"; return false; }
  const Tlv* v = firstChild(t, {0x5A});
  if (!v || v->value.empty()) { msg = "eUICC 未返回 EID"; return false; }
  eid = eidDecode(v->value);
  return true;
}

// ES10c GetProfilesInfo: BF2D [5C 08 5A 4F 9F 70 90 91 92 95]
bool getProfilesRaw(int ch, bool withTags, std::vector<uint8_t>& raw, String& msg) {
  std::vector<uint8_t> body;
  if (withTags) {
    std::vector<uint8_t> tags = {0x5A, 0x4F, 0x9F, 0x70, 0x90, 0x91, 0x92, 0x95};
    appendTlv(body, {0x5C}, tags);
  }
  std::vector<uint8_t> req;
  appendTlv(req, {0xBF, 0x2D}, body);
  return storeData(ch, req, raw, msg);
}

bool parseProfiles(const std::vector<uint8_t>& raw, std::vector<EsimProfile>& out, String& msg) {
  Tlv t;
  if (!parseTlv(raw, t, msg)) return false;
  if (!tagIs(t, {0xBF, 0x2D})) { msg = "Profile 列表响应 tag 不匹配"; return false; }
  if (const Tlv* err = firstChild(t, {0x81})) {
    msg = (tlvInt(*err, 127) == 1) ? "eUICC 拒绝 Profile 列表查询参数" : "eUICC 返回 Profile 列表错误";
    return false;
  }
  const Tlv* container = firstChild(t, {0xA0});
  const std::vector<Tlv>& rows = container ? container->children : t.children;
  out.clear();
  for (const Tlv& row : rows) {
    if (!tagIs(row, {0xE3}) && !tagIs(row, {0xBF, 0x25})) continue;
    EsimProfile p;
    if (const Tlv* v = firstChild(row, {0x5A})) p.iccid = bcdDecode(v->value);
    if (const Tlv* v = firstChild(row, {0x4F})) p.isdpAid = bytesToHex(v->value);
    if (const Tlv* v = firstChild(row, {0x9F, 0x70})) {
      int s = tlvInt(*v, -1);
      p.state = (s == 1) ? "enabled" : (s == 0 ? "disabled" : "unknown");
    }
    if (const Tlv* v = firstChild(row, {0x90})) p.nickname = String((const char*)v->value.data(), v->value.size());
    if (const Tlv* v = firstChild(row, {0x91})) p.serviceProvider = String((const char*)v->value.data(), v->value.size());
    if (const Tlv* v = firstChild(row, {0x92})) p.profileName = String((const char*)v->value.data(), v->value.size());
    if (const Tlv* v = firstChild(row, {0x95})) {
      int c = tlvInt(*v, 1);
      p.profileClass = (c == 0) ? "test" : (c == 1 ? "provisioning" : (c == 2 ? "operational" : "unknown"));
    }
    if (p.state.length() == 0) p.state = "disabled";
    if (p.profileClass.length() == 0) p.profileClass = "provisioning";
    if (p.iccid.length() > 0 || p.isdpAid.length() > 0) out.push_back(p);
  }
  return true;
}

bool getProfiles(int ch, std::vector<EsimProfile>& out, String& msg) {
  std::vector<uint8_t> raw;
  String firstErr;
  if (getProfilesRaw(ch, true, raw, msg) && parseProfiles(raw, out, msg)) return true;
  firstErr = msg;

  raw.clear();
  msg = "";
  if (getProfilesRaw(ch, false, raw, msg) && parseProfiles(raw, out, msg)) return true;

  if (firstErr.length() > 0) msg = firstErr + "；兼容查询仍失败: " + msg;
  return false;
}

// ICCID 数字串 -> GSM BCD（10 字节，0xFF 填充）
bool encodeIccid(const String& digits, std::vector<uint8_t>& out) {
  if (digits.length() < 18 || digits.length() > 22) return false;
  out.clear();
  for (size_t i = 0; i < digits.length(); i += 2) {
    uint8_t lo = (uint8_t)(digits[i] - '0');
    uint8_t hi = (i + 1 < digits.length()) ? (uint8_t)(digits[i + 1] - '0') : 0x0F;
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  while (out.size() < 10) out.push_back(0xFF);
  return true;
}

bool allDigits(const String& s) {
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

// ES10c EnableProfile: BF31 { A0 { 5A iccid | 4F aid } , 81 01 00 }
bool enableOnCard(int ch, const String& identifier, String& msg) {
  std::vector<uint8_t> idTlv;
  if (allDigits(identifier) && identifier.length() >= 18 && identifier.length() <= 22) {
    std::vector<uint8_t> bcd;
    if (!encodeIccid(identifier, bcd)) { msg = "ICCID 格式无效"; return false; }
    appendTlv(idTlv, {0x5A}, bcd);
  } else {
    std::vector<uint8_t> aid;
    if (!hexToBytes(identifier, aid) || aid.size() < 5 || aid.size() > 32) {
      msg = "Profile 标识无效（需要 ICCID 或 ISD-P AID）";
      return false;
    }
    appendTlv(idTlv, {0x4F}, aid);
  }

  std::vector<uint8_t> body;
  appendTlv(body, {0xA0}, idTlv);
  appendTlv(body, {0x81}, {0x00});  // refreshFlag = FALSE

  std::vector<uint8_t> req;
  appendTlv(req, {0xBF, 0x31}, body);

  std::vector<uint8_t> raw;
  if (!storeData(ch, req, raw, msg)) return false;

  Tlv t;
  if (!parseTlv(raw, t, msg)) return false;
  if (!tagIs(t, {0xBF, 0x31})) { msg = "EnableProfile 响应 tag 不匹配"; return false; }
  const Tlv* result = firstChild(t, {0x80});
  if (!result) { msg = "EnableProfile 响应缺少结果码"; return false; }
  int code = tlvInt(*result, 127);
  if (code != 0) {
    switch (code) {
      case 1: msg = "ICCID/AID 不存在"; break;
      case 2: msg = "Profile 不是禁用状态"; break;
      case 3: msg = "Profile 策略禁止该操作"; break;
      case 4: msg = "卡拒绝重复启用当前 Profile"; break;
      case 5: msg = "卡工具包忙，请稍后重试"; break;
      default: msg = "eUICC 返回错误码 " + String(code); break;
    }
    return false;
  }
  return true;
}

}  // namespace

bool Esim::query(String& eid, std::vector<EsimProfile>& out, bool& isEuicc, String& message) {
  eid = "";
  out.clear();
  isEuicc = false;

  int ch = 0;
  if (!openChannel(ch, message)) return false;
  isEuicc = true;

  String eidMsg, listMsg;
  bool eidOk  = getEid(ch, eid, eidMsg);
  bool listOk = getProfiles(ch, out, listMsg);
  closeChannel(ch);

  if (eidOk && listOk) {
    message = "查询成功";
    return true;
  }
  if (listOk) {
    message = "Profile 已读取，但 EID 读取失败: " + eidMsg;
    return true;
  }
  if (eidOk) {
    message = "EID 已读取，但 Profile 列表失败: " + listMsg;
    return false;
  }
  message = eidMsg + "；" + listMsg;
  return false;
}

bool Esim::enableProfile(const String& identifier, String& message) {
  if (identifier.length() == 0) {
    message = "缺少 Profile 标识";
    return false;
  }
  int ch = 0;
  if (!openChannel(ch, message)) return false;
  bool ok = enableOnCard(ch, identifier, message);
  closeChannel(ch);
  if (ok) {
    message = "Profile 已启用，设备将在 2 秒后自动重启";
  }
  return ok;
}
