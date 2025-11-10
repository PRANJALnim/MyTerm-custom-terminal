// Enable POSIX functions (getline, strdup) on glibc
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "history.h"
#include "myterm.h"

#ifndef HISTORY_MAX
#define HISTORY_MAX 10000
#endif

static char *history[HISTORY_MAX];
static size_t history_count = 0;
static char history_path[512];

static void history_set_path(void) {
    const char *home = getenv("HOME");
    if (!home) { history_path[0] = '\0'; return; }
    snprintf(history_path, sizeof(history_path), "%s/.myterm_history", home);
}

void history_init(void) {
    history_set_path();
}

void history_load(void) {
    history_set_path();
    if (!*history_path) return;
    FILE *f = fopen(history_path, "r"); if (!f) return;
    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (n>0 && (line[n-1]=='\n' || line[n-1]=='\r')) line[n-1]='\0';
        add_history(line);
    }
    free(line); fclose(f);
}

void history_save_append(const char *line) {
    if (!*history_path) return;
    FILE *f = fopen(history_path, "a"); if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

void add_history(const char *line) {
    if (!line || !*line) return;
    if (history_count == HISTORY_MAX) {
        free(history[0]);
        memmove(&history[0], &history[1], sizeof(char*) * (HISTORY_MAX - 1));
        history_count--;
    }
    history[history_count++] = strdup(line);
}

static int longest_common_substr_len(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    int best = 0;
    for (int i=0;i<la;i++) for (int j=0;j<lb;j++) {
        int k=0; while (i+k<la && j+k<lb && a[i+k]==b[j+k]) k++;
        if (k>best) best=k;
    }
    return best;
}

void print_history_command(void) {
    size_t start = history_count > 1000 ? history_count - 1000 : 0;
    for (size_t i = start; i < history_count; i++) {
        char line[128];
        int n = snprintf(line, sizeof(line), "%zu  ", i + 1);
        append_output(line, (size_t)n);
        append_output_str(history[i]);
        append_output_str("\n");
    }
}

void history_search_and_print(const char *term) {
    for (ssize_t i=(ssize_t)history_count-1;i>=0;i--) {
        if (strcmp(history[i], term)==0) {
            append_output_str(history[i]);
            append_output_str("\n");
            return;
        }
    }
    int best = 0;
    for (size_t i=0;i<history_count;i++) {
        int l = longest_common_substr_len(history[i], term);
        if (l>best) best=l;
    }
    if (best>2) {
        for (size_t i=0;i<history_count;i++) {
            int l = longest_common_substr_len(history[i], term);
            if (l==best) { append_output_str(history[i]); append_output_str("\n"); }
        }
    } else {
        append_output_str("No match for search term in history\n");
    }
}
