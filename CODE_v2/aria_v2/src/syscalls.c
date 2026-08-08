/* Minimal syscall stubs for bare-metal FreeRTOS build.
 *
 * The linker reports undefined references to exactly `exit`, `kill`, and
 * `getpid` (no leading underscore) — pulled in via libc_a-abort.o and
 * libc_a-signalr.o when this nano-newlib build's abort()/assert() path
 * runs. These are not the traditional _exit/_kill/_getpid retarget hooks;
 * this toolchain variant calls the plain names directly, so that's what
 * must be defined here.
 *
 * None of these are expected to run in normal operation — they exist only
 * to satisfy the linker in case an assert() or abort() is ever triggered.
 *
 * Additionally, depending on which Newlib object files get pulled into the
 * link (this can shift as build settings change, e.g. enabling
 * configSUPPORT_DYNAMIC_ALLOCATION), some paths instead reference the
 * traditional underscored retarget hooks (_exit, _getpid, _kill). Both sets
 * are defined here so the link succeeds regardless of which variant is
 * requested; the underscored versions simply forward to the real
 * implementations above to avoid duplicating logic.
 */
#include <errno.h>

int getpid(void) {
    return 1;
}

int kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

void exit(int status) {
    (void)status;
    while (1) {
        /* Trap here forever on abort(). A real target should reset instead. */
    }
}

/* Underscored retarget hooks — forward to the implementations above. */
void _exit(int status) {
    exit(status);
}

int _getpid(void) {
    return getpid();
}

int _kill(int pid, int sig) {
    return kill(pid, sig);
}
