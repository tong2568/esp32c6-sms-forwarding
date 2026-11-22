#pragma once
#include <Arduino.h>
#include <vector>

struct EsimProfile {
  String iccid;
  String isdpAid;
  String state;           // enabled / disabled / unknown
  String nickname;
  String serviceProvider;
  String profileName;
  String profileClass;    // test / provisioning / operational
};

// 实体 eUICC（eSIM 卡）查询与 Profile 启用。
// 通过模组的 AT+CCHO / AT+CGLA / AT+CCHC（ISO7816 逻辑通道 + APDU）访问卡内 ISD-R，
// 再按 SGP.22 ES10c 解析 EID / Profile 列表 / EnableProfile。
class Esim {
public:
  // 查询 EID 与 Profile 列表。
  // isEuicc=false 表示当前卡不是实体 eSIM（或模组不支持 CCHO/CGLA）。
  static bool query(String& eid, std::vector<EsimProfile>& out, bool& isEuicc, String& message);

  // 启用指定 Profile（iccid 或 isdpAid）。成功后需重启模组/设备使新 Profile 生效。
  static bool enableProfile(const String& identifier, String& message);
};
