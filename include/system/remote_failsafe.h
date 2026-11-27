#ifndef __system__remote_failsafe_h
#define __system__remote_failsafe_h

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Stop the motor if a remote controller (WebSocket "setMotor" or USB TS-code S03) set it
// running and then went silent. The controlling app re-sends its value about every 1.5 s;
// if nothing arrives for REMOTE_FAILSAFE_TIMEOUT_MS the motor is switched off. A later
// remote command simply sets it again.
#define REMOTE_FAILSAFE_TIMEOUT_MS 4000

void remote_failsafe_init(void);
void remote_failsafe_note(uint8_t speed);

#ifdef __cplusplus
}
#endif

#endif
