#ifndef __system__action_manager_c
#define __system__action_manager_c

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

void action_manager_init(void);
void action_manager_load_plugin(const char* filename);
void action_manager_load_all_plugins(void);
void action_manager_dispatch_event(const char* event, int val);

size_t action_manager_get_plugin_count(void);

// NOTE: The WASM action-plugin system is compiled out of this build (the mt-actions
// library it depends on is private). See src/system/no_plugins.c.

#ifdef __cplusplus
}
#endif

#endif
