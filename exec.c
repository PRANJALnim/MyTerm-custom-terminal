#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <poll.h>
#include "myterm.h"
#include "exec.h"

static int is_whitespace(char c) { return c==' '||c=='\t' || c=='\n'; }

static char *trim(char *s) {
    while (*s && is_whitespace(*s)) s++;
    size_t len = strlen(s);
    while (len>0 && is_whitespace(s[len-1])) { s[--len] = '\0'; }
    return s;
}

int parse_args(char *cmd, char *argv[], char **infile, char **outfile, int *append) {
    int argc = 0; *infile = NULL; *outfile = NULL; *append = 0;
    char *p = cmd;
    while (*p) {
        while (is_whitespace(*p)) p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            argv[argc++] = p;
            while (*p && *p != quote) p++;
            if (*p) *p++ = '\0';
        } else if (*p == '<') {
            p++;
            while (is_whitespace(*p)) p++;
            *infile = p;
            while (*p && !is_whitespace(*p)) p++;
            if (*p) *p++ = '\0';
        } else if (*p == '>') {
            p++;
            if (*p == '>') { *append = 1; p++; }
            while (is_whitespace(*p)) p++;
            *outfile = p;
            while (*p && !is_whitespace(*p)) p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && !is_whitespace(*p) && *p!='<' && *p!='>') p++;
            if (*p) { *p++ = '\0'; }
        }
        if (argc >= MAX_ARGS-1) break;
    }
    argv[argc] = NULL;
    return argc;
}

int split_pipes(char *line, char *stages[]) {
    int n = 0; char *p = line; stages[n++] = p;
    while (*p) {
        if (*p == '|') { *p = '\0'; if (n < MAX_PIPE) stages[n++] = p + 1; }
        p++;
    }
    return n;
}

int execute_pipeline(char *line, int background) {
    // support built-in 'cd' and 'history' when no pipe
    char tmp[MAX_INPUT]; strncpy(tmp, line, sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
    char *trimmed = trim(tmp);
    // single built-ins
    if (!strchr(trimmed, '|')) {
        if (strncmp(trimmed, "cd ", 3) == 0 || strcmp(trimmed, "cd") == 0) {
            const char *path = trimmed[2] ? trimmed + 3 : getenv("HOME");
            if (!path) path = ".";
            if (chdir(path) != 0) append_output_str("cd: failed\n");
            return 0;
        }
        if (strcmp(trimmed, "history") == 0) {
            extern void print_history_command(void);
            print_history_command();
            return 0;
        }
        if (strcmp(trimmed, "clear") == 0) {
            // Clear the output buffer of current tab
            extern void clear_screen(void);
            clear_screen();
            return 0;
        }
        if (strcmp(trimmed, "help") == 0) {
            append_output_str("\n");
            append_output_str("MyTerm\n");
            append_output_str("=================================\n\n");
            append_output_str("Built-in Commands:\n");
            append_output_str("  cd [dir]          Change directory\n");
            append_output_str("  clear             Clear the screen\n");
            append_output_str("  history           Show command history\n");
            append_output_str("  jobs              Show background jobs\n");
            append_output_str("  help              Show this help message\n");
            append_output_str("  multiWatch [...]  Run commands in parallel\n");
            append_output_str("\n");
            append_output_str("I/O Redirection:\n");
            append_output_str("  cmd < file        Redirect input from file\n");
            append_output_str("  cmd > file        Redirect output to file (overwrite)\n");
            append_output_str("  cmd >> file       Redirect output to file (append)\n");
            append_output_str("  cmd1 | cmd2       Pipe output to next command\n");
            append_output_str("  cmd &             Run command in background\n");
            append_output_str("\n");
            append_output_str("Keyboard Shortcuts:\n");
            append_output_str("  Ctrl+A            Move cursor to line start\n");
            append_output_str("  Ctrl+E            Move cursor to line end\n");
            append_output_str("  Ctrl+C            Interrupt running command\n");
            append_output_str("  Ctrl+Z            Move command to background\n");
            append_output_str("  Ctrl+R            Search command history\n");
            append_output_str("  Ctrl+T            Create new tab\n");
            append_output_str("  Tab               Auto-complete filename\n");
            append_output_str("  Shift+Enter       Insert newline (multiline input)\n");
            append_output_str("  PageUp/PageDown   Scroll output\n");
            append_output_str("\n");
            append_output_str("Examples:\n");
            append_output_str("  ls -la | grep txt\n");
            append_output_str("  sort < input.txt > output.txt\n");
            append_output_str("  gcc -o prog prog.c && ./prog\n");
            append_output_str("  multiWatch [\"cmd1\", \"cmd2\"]\n");
            append_output_str("\n");
            return 0;
        }
        if (strcmp(trimmed, "jobs") == 0) {
            extern int cap_active;
            extern pid_t fg_child;
            extern BackgroundJob bg_jobs[];
            extern int bg_job_count;
            
            int shown = 0;
            
            // Show foreground job if active
            if (cap_active && fg_child > 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "[fg]  Running                 (pid %d)\n", fg_child);
                append_output_str(msg);
                shown++;
            }
            
            // Show background jobs and check their status
            for (int i = 0; i < MAX_JOBS; i++) {
                if (bg_jobs[i].active) {
                    // Check if process still exists
                    int alive = 0;
                    for (int j = 0; j < bg_jobs[i].nprocs; j++) {
                        if (bg_jobs[i].pids[j] > 0) {
                            int status;
                            pid_t result = waitpid(bg_jobs[i].pids[j], &status, WNOHANG);
                            if (result == 0) {
                                alive = 1;  // Still running
                            } else if (result == bg_jobs[i].pids[j]) {
                                bg_jobs[i].pids[j] = 0;  // Process finished
                            }
                        }
                    }
                    
                    if (alive) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "[%d]  Running                 %s\n", 
                                i + 1, bg_jobs[i].command);
                        append_output_str(msg);
                        shown++;
                    } else {
                        // All processes done, mark job as inactive
                        bg_jobs[i].active = 0;
                        bg_job_count--;
                    }
                }
            }
            
            if (shown == 0) {
                append_output_str("No jobs running\n");
            }
            return 0;
        }
    }

    // build pipeline
    char *stages[MAX_PIPE];
    int nstages = split_pipes(line, stages);
    int pipes_fd[MAX_PIPE-1][2];
    for (int i=0;i<nstages-1;i++) if (pipe(pipes_fd[i])<0) die("pipe");

    // determine if parent should capture stdout from last stage (no explicit outfile), and always capture stderr
    int out_pipe[2] = {-1,-1};
    int err_pipe[2] = {-1,-1};
    {
        char lastbuf[MAX_INPUT]; strncpy(lastbuf, trim(stages[nstages-1]), sizeof(lastbuf)-1); lastbuf[sizeof(lastbuf)-1]='\0';
        char *infile=NULL,*outfile=NULL; int app=0; char *argv_dummy[MAX_ARGS];
        parse_args(lastbuf, argv_dummy, &infile, &outfile, &app);
        if (outfile == NULL) {
            if (pipe(out_pipe) < 0) die("pipe");
        }
        if (pipe(err_pipe) < 0) die("pipe");
    }

    pid_t pids[MAX_PIPE];
    for (int i=0;i<nstages;i++) {
        char *infile=NULL,*outfile=NULL; int app=0; char *argv[MAX_ARGS];
        char stagebuf[MAX_INPUT];
        strncpy(stagebuf, trim(stages[i]), sizeof(stagebuf)-1); stagebuf[sizeof(stagebuf)-1]='\0';
        parse_args(stagebuf, argv, &infile, &outfile, &app);
        pid_t pid = fork();
        if (pid<0) die("fork");
        if (pid==0) {
            // child
            // set up stdin/stdout
            if (i>0) { dup2(pipes_fd[i-1][0], STDIN_FILENO); }
            if (i<nstages-1) { dup2(pipes_fd[i][1], STDOUT_FILENO); }
            // close all pipe fds
            for (int k=0;k<nstages-1;k++){ close(pipes_fd[k][0]); close(pipes_fd[k][1]); }
            // redirections
            if (infile) {
                int fd = open(infile, O_RDONLY);
                if (fd<0) _exit(127);
                dup2(fd, STDIN_FILENO); close(fd);
            }
            if (outfile) {
                int flags = O_WRONLY|O_CREAT|(app?O_APPEND:O_TRUNC);
                int fd = open(outfile, flags, 0644);
                if (fd<0) _exit(127);
                dup2(fd, STDOUT_FILENO); close(fd);
            } else if (i == nstages-1 && out_pipe[1] != -1) {
                // last stage: capture stdout to parent
                dup2(out_pipe[1], STDOUT_FILENO);
            }
            // always capture stderr of last stage
            if (i == nstages-1 && err_pipe[1] != -1) {
                dup2(err_pipe[1], STDERR_FILENO);
            }
            if (out_pipe[0] != -1) close(out_pipe[0]);
            if (out_pipe[1] != -1) close(out_pipe[1]);
            if (err_pipe[0] != -1) close(err_pipe[0]);
            if (err_pipe[1] != -1) close(err_pipe[1]);
            execvp(argv[0], argv);
            // exec failed: write error to stderr (captured by parent if enabled)
            fprintf(stderr, "myterm: %s: %s\n", argv[0] ? argv[0] : "(null)", strerror(errno));
            _exit(127);
        }
        pids[i] = pid;
    }
    // parent closes all pipeline intermediate fds
    for (int k=0;k<nstages-1;k++){ close(pipes_fd[k][0]); close(pipes_fd[k][1]); }
    // close write ends of capture pipes in parent, keep read ends and make them nonblocking
    if (out_pipe[1] != -1) close(out_pipe[1]);
    if (err_pipe[1] != -1) close(err_pipe[1]);
    if (out_pipe[0] != -1) { int fl = fcntl(out_pipe[0], F_GETFL, 0); fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK); }
    if (err_pipe[0] != -1) { int fl = fcntl(err_pipe[0], F_GETFL, 0); fcntl(err_pipe[0], F_SETFL, fl | O_NONBLOCK); }

    // register capture state and return immediately; main loop will pump IO
    cap_out_fd = out_pipe[0];
    cap_err_fd = err_pipe[0];
    for (int i=0;i<nstages;i++) cap_pids[i] = pids[i];
    cap_nstages = nstages;
    cap_active = 1;
    fg_child = pids[nstages-1];
    (void)background;
    return 0;
}
