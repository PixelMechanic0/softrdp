#ifndef PJ64_LOG_H
#define PJ64_LOG_H

#include <stdbool.h>

void pj64_log_open(void);
void pj64_log_close(void);
void pj64_log_printf(const char *fmt, ...);
bool pj64_log_is_open(void);

#endif
