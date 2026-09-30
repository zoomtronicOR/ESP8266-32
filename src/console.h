#pragma once

// Line-based serial console (115200 baud), mainly for provisioning over USB:
//   status                        WiFi / scan state
//   wifi "<ssid>" "<password>"    join a network (same path as the web WiFi page)
//   reboot | factory | help
namespace Console {

void loop();

}  // namespace Console
