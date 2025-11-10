#ifndef EXEC_H
#define EXEC_H

#include "myterm.h"

int parse_args(char *cmd, char *argv[], char **infile, char **outfile, int *append);
int split_pipes(char *line, char *stages[]);
int execute_pipeline(char *line, int background);

#endif // EXEC_H
