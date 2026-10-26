/* camwall-planes: helpers shared by the stream and display sides. */
#ifndef CAMWALL_COMMON_H
#define CAMWALL_COMMON_H

#include <signal.h>
#include <stdint.h>

#define MAXCAM 4

extern volatile sig_atomic_t g_stop;   /* set by SIGINT/SIGTERM */

int64_t now_ms(void);                  /* CLOCK_MONOTONIC in milliseconds */
void say(const char *who, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
