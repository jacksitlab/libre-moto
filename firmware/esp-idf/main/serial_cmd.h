/* serial_cmd.h */
#ifndef SERIAL_CMD_H
#define SERIAL_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

void serial_cmd_init(void);
void serial_cmd_poll(void);

/* Implemented in app_main.cpp (needs the app's screens/state). */
void serial_cmd_handle_line(const char *line);

#ifdef __cplusplus
}
#endif

#endif
