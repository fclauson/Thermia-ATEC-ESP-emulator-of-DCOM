#pragma once
// registers_json_handler.h  v2026.10.20.4  serves GET /registers.json (ESPHome web_server_base)
//
// Runs in the web server's own task, NOT the main loop, so it never touches the register
// store or the live map directly: it only copies the JSON text the main loop last built
// (ThermiaGateway::registers_json(), mutex-protected, refreshed every 30 s). Read-only:
// there is no way to change anything through this endpoint.

#include <cstring>
#include <string>
#include "esphome/components/web_server_base/web_server_base.h"
#include "thermia_gateway.h"
#ifdef USE_ESP_IDF
#include <esp_http_server.h>
#endif

namespace esphome {
namespace thermia_gateway {

class RegistersJsonHandler : public AsyncWebHandler {
 public:
  explicit RegistersJsonHandler(ThermiaGateway *parent) : parent_(parent) {}

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
    if (request->method() != HTTP_GET) return false;
    const std::string uri = uri_of_(request, 0);
    const size_t q = uri.find('?');  // ignore any query string
    return uri.compare(0, q, "/registers.json") == 0;
  }

  // ESPHome's request API changed between versions (url() was removed in newer ESP-IDF
  // builds). Overload resolution picks whichever works: url() if it exists (int beats long),
  // otherwise the raw ESP-IDF request, whose 'uri' field has been stable for years.
  template<typename R> static auto uri_of_(R *r, int) -> decltype(r->url(), std::string()) {
    return std::string(r->url().c_str());
  }
#ifdef USE_ESP_IDF
  template<typename R>
  static auto uri_of_(R *r, long) -> decltype(static_cast<httpd_req_t *>(*r), std::string()) {
    const httpd_req_t *req = static_cast<httpd_req_t *>(*r);
    return req != nullptr ? std::string(req->uri) : std::string();
  }
#endif
  ThermiaGateway *parent_;
};

}  // namespace thermia_gateway
}  // namespace esphome
