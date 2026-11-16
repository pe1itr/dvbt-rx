/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LINRAD_PROCESS_CONTROL_H
#define LINRAD_PROCESS_CONTROL_H
#include <glib.h>
#include <signal.h>
#ifndef _WIN32
#include <unistd.h>
#endif
static inline void process_stop(GPid pid, gboolean group, gboolean force) {
#ifdef _WIN32
    (void)group; (void)force;
    lw_stop_process(pid);
#else
    if (pid>0) kill(group ? -pid : pid,force ? SIGKILL : SIGTERM);
#endif
}
#endif
