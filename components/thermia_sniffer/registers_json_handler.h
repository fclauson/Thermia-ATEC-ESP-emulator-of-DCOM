#pragma once
// registers_json_handler.h  v2026.10.20.3  serves GET /registers.json (ESPHome web_server_base)
//
// Runs in the web server's own task, NOT the main loop, so it never touches the register
// store or the live map directly: it only copies the JSON text the main loop last built
// (ThermiaSniffer::registers_json(), mutex-protected, refreshed every 30 s). Read-only:
// there is no way to change anything through this endpoint.

#include "esphome/components/web_server_base/web_server_base.h"
#include "thermia_sniffer.h"

namespace esphome {
namespace thermia_sniffer {

class RegistersJsonHandler : public AsyncWebHandler {
 public:
  explicit RegistersJsonHandler(ThermiaSniffer *parent) : parent_(parent) {}

  // ESPHome versions differ on whether canHandle is const; both are declared (no 'override')
  // so whichever the installed version uses is the one that overrides.
  bool canHandle(AsyncWebServerRequest *request) const { return matches_(request); }
  bool canHandle(AsyncWebServerRequest *request) { return matches_(request); }

  void handleRequest(AsyncWebServerRequest *request) {
    const std::string body = this->parent_->registers_json();
    request->send(200, "application/json", body.c_str());
  }

 protected:
  static bool matches_(AsyncWebServerRequest *request) {
    return request->method() == HTTP_GET && request->url() == "/registers.json";
  }
  ThermiaSniffer *parent_;
};

}  // namespace thermia_sniffer
}  // namespace esphome
