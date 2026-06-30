/*
 * CrossDesk Core — in-tree-only C++ accessors.
 *
 * NOT part of the stable C ABI. Only used by legacy crossdesk binary and
 * future crossdesk_session to access the underlying C++ objects owned by
 * the core during the migration. Flutter never includes this header.
 *
 * Linking: requires crossdesk_core shared library; symbols are exported
 * the same way as the C ABI.
 */

#ifndef CROSSDESK_CORE_INTERNAL_H_
#define CROSSDESK_CORE_INTERNAL_H_

#include "crossdesk_core.h"

// Forward decls so callers don't need the full headers.
namespace crossdesk { class ConfigCenter; }
class DevicePresence;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns non-owning pointer; valid until cd_core_destroy(core). */
CD_API crossdesk::ConfigCenter* cd_internal_get_config_center(cd_core_t* core);
CD_API DevicePresence*          cd_internal_get_device_presence(cd_core_t* core);

#ifdef __cplusplus
}
#endif

#endif  /* CROSSDESK_CORE_INTERNAL_H_ */
