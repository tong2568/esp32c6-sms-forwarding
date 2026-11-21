#include "logs.h"
#include "../../logger/logger.h"

void logsController(AsyncWebServerRequest* request) {
  int n = Logger::DUMP_DEFAULT_LINES;
  if (request->hasParam("n")) {
    n = request->getParam("n")->value().toInt();
    if (n <= 0) n = Logger::DUMP_DEFAULT_LINES;
  }

  auto fp = Logger::dumpStream(n);
  if (!fp || !*fp) {
    // 未开启「写入日志文件」或文件尚未生成：回退到内存环形缓冲，
    // 保证 /api/logs 始终能看到最近若干条日志，而不是空白页。
    String mem = Logger::dumpMemory(n);
    if (mem.length() == 0) mem = "(暂无日志)\n";
    request->send(200, "text/plain; charset=utf-8", mem);
    return;
  }
  // lambda 按值捕获 shared_ptr：响应期间引用计数 >= 1，句柄不会提前关闭；
  // lambda 随 response 析构时 shared_ptr 引用计数归零，File 析构自动 close()。
  auto* response = request->beginChunkedResponse(
    "text/plain; charset=utf-8",
    [fp](uint8_t* buf, size_t maxLen, size_t) -> size_t {
      if (!fp->available()) { fp->close(); return 0; }
      return fp->read(buf, maxLen);
    }
  );
  request->send(response);
}

void logsDeleteController(AsyncWebServerRequest* request) {
  Logger::clearFile();
  request->send(200, "application/json", "{\"ok\":true}");
}
