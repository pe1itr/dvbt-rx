#ifndef LINRAD_SRT_OUTPUT_H
#define LINRAD_SRT_OUTPUT_H
#include <glib.h>

/* Validate a caller destination without opening a network connection. */
gboolean srt_output_validate_url(const char *url, GError **error);
gboolean srt_output_valid_preset(const char *preset);
/* Supervise FFmpeg with TS on stdin; no GTK/display or SDR dependencies. */
int srt_output_main(int argc, char **argv);
#endif
