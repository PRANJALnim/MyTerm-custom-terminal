#ifndef MYTERM_H
#define MYTERM_H

#include <sys/types.h>

// Forward declaration for X11 Display type
typedef struct _XDisplay Display;

#include <stddef.h>
#include <sys/types.h>

// Shared constants
#ifndef BUF_SIZE
#define BUF_SIZE 8192
#endif
#ifndef MAX_INPUT
#define MAX_INPUT 1024
#endif
#ifndef MAX_ARGS
#define MAX_ARGS 128
#endif
#ifndef MAX_PIPE
#define MAX_PIPE 16
#endif

// Provided by UI (src/main.c)
void append_output(const char *s, size_t n);
void append_output_str(const char *s);
void die(const char *msg);
// UI repaint
void draw(void);

// Foreground capture state (defined in src/main.c, used by exec.c)
extern int cap_out_fd;
extern int cap_err_fd;
extern pid_t cap_pids[MAX_PIPE];
extern int cap_nstages;
extern int cap_active;
extern pid_t fg_child;

// Interrupt flag for Ctrl+C (set by main loop, checked by blocking operations)
extern volatile int interrupt_requested;

// X11 display for event checking in blocking operations (defined in main.c)
extern Display *dpy_global;

// Background job tracking
#define MAX_JOBS 64
typedef struct {
    int active;
    pid_t pids[MAX_PIPE];
    int nprocs;
    char command[256];
} BackgroundJob;

extern BackgroundJob bg_jobs[MAX_JOBS];
extern int bg_job_count;

#endif // MYTERM_H
