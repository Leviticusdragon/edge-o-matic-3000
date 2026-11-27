// Stand-ins for the WASM action-plugin system and the BLE plugin driver.
//
// The upstream firmware links against MausTec/mt-actions, which is a private repository,
// so it cannot be built outside Maus-Tec. This build compiles the plugin system out:
// SD-card .wasm plugins and plugin-driven BLE accessories are unavailable; everything
// else (edging, Wi-Fi/WebSocket API, serial, menus, updates) is unchanged.

#include "drivers/plugin_driver.h"
#include "system/action_manager.h"

void action_manager_init(void) {}
void action_manager_load_plugin(const char* filename) { (void)filename; }
void action_manager_load_all_plugins(void) {}
void action_manager_dispatch_event(const char* event, int val) {
    (void)event;
    (void)val;
}
size_t action_manager_get_plugin_count(void) { return 0; }

void plugin_driver_init(void) {}
bool plugin_driver_try_claim(peer_t* peer) {
    (void)peer;
    return false;
}
void plugin_driver_release_peer(peer_t* peer) { (void)peer; }
void plugin_driver_tick(void) {}
void plugin_driver_enumerate(plugin_driver_enum_cb_t cb, void* arg) {
    (void)cb;
    (void)arg;
}
bool plugin_driver_can_handle_name(const char* name) {
    (void)name;
    return false;
}
