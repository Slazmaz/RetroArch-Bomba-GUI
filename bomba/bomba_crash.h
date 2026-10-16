#ifndef __BOMBA_CRASH_H
#define __BOMBA_CRASH_H

/* Crash reports for players to send in: on a crash, the install's logs
 * folder (next to app\, where the launch script keeps RetroArch's logs)
 * gets crash-<time>.txt, what crashed and the stack as module+offset, and
 * crash-<time>.dmp, a minidump. Call first thing at startup. Windows only;
 * elsewhere it does nothing. */
void bomba_crash_init(void);

#endif
