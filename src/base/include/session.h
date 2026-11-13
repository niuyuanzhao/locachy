/* App-owned session policy. The descriptor is created by process 1, never
 * opened by pathname or passed to a network worker. Calls fail closed without
 * it. Poll base_session_fd alongside input; a change requires input-cancel. */
#ifndef LOCACHY_SESSION_H
#define LOCACHY_SESSION_H

int base_session_fd(void);
/* 1: policy changed (cancel input), 0: unchanged, -1: owner channel gone. */
int base_session_service(void);
int base_session_input_allowed(void);
/* Call only after all remote keys/buttons and queued input have been cleared. */
int base_session_input_cancelled(void);
/* Idempotent, owned by this app generation; not inherited by children. */
int base_sleep_inhibit(int enabled);
/* Authorized temporary output power, never a saved brightness preference. */
int base_display_power(int on);
/* A live link to another computer (extdisplay): while held, process 1 takes
 * the screen away for home or the lock but does not freeze this program, so
 * the link stays up; remote input still follows the policy. 0 drops it. A
 * report, not a request: no answer is awaited. */
int base_session_hold(int on);
/* Implemented by the display backend. Missing/unsupported is an error. */
int base_screen_power(int on) __attribute__((weak));

#endif
