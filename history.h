#ifndef HISTORY_H
#define HISTORY_H

#include <stddef.h>

void history_init(void);
void history_load(void);
void history_save_append(const char *line);
void add_history(const char *line);
void print_history_command(void);
void history_search_and_print(const char *term);

#endif // HISTORY_H
