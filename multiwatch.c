// Enable POSIX clock_gettime on glibc
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "myterm.h"
#include "multiwatch.h"

void multiwatch_run(char *argstr) {
    // Expect format: ["cmd1", "cmd2", ...]
    // crude parse: extract quoted strings
    char *p = argstr;
    char *cmds[64]; int ncmd=0;
    while (*p && ncmd < 64) {
        while (*p && *p!='"') p++;
        if (!*p) break;
        p++;
        cmds[ncmd++] = p;
        while (*p && *p!='"') p++;
        if (*p) *p++='\0';
    }
    if (ncmd==0) { append_output_str("multiWatch: no commands\n"); return; }

    // create pipes and child processes
    int rfd[64]; pid_t pids[64];
    for (int i=0;i<ncmd;i++) {
        int pipefd[2];
        if (pipe(pipefd) < 0) { append_output_str("multiWatch: pipe failed\n"); return; }
        pid_t pid = fork();
        if (pid==0) {
            // child: redirect stdout/stderr to pipe write end and exec via sh -c
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            for (int k=3;k<256;k++) close(k);
            execl("/bin/sh", "sh", "-c", cmds[i], (char*)NULL);
            _exit(127);
        }
        // parent
        close(pipefd[1]);
        int fl = fcntl(pipefd[0], F_GETFL, 0); fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);
        rfd[i] = pipefd[0];
        pids[i] = pid;
    }

    struct pollfd pfds[64];
    for (int i=0;i<ncmd;i++) { pfds[i].fd = rfd[i]; pfds[i].events = POLLIN | POLLHUP; }
    int open_streams = ncmd;
    append_output_str("multiWatch started. Press Ctrl+C to stop.\n");
    while (open_streams>0) {
        // Check X11 events for Ctrl+C and Ctrl+Z
        while (XPending(dpy_global)) {
            XEvent ev;
            XNextEvent(dpy_global, &ev);
            if (ev.type == KeyPress) {
                KeySym ks;
                char kb[8];
                XLookupString(&ev.xkey, kb, sizeof(kb), &ks, NULL);
                if ((ev.xkey.state & ControlMask) && (ks == XK_c || ks == XK_C)) {
                    interrupt_requested = 1;
                }
                if ((ev.xkey.state & ControlMask) && (ks == XK_z || ks == XK_Z)) {
                    // Ctrl+Z: suspend all processes and return
                    append_output_str("\n^Z\n[multiWatch processes suspended and moved to background]\n");
                    for (int i=0;i<ncmd;i++) {
                        if (pids[i] > 0) {
                            kill(pids[i], SIGTSTP);  // suspend, don't kill
                        }
                    }
                    // Close pipes and return (processes remain suspended in background)
                    for (int i=0;i<ncmd;i++) {
                        if (pfds[i].fd >= 0) { close(pfds[i].fd); pfds[i].fd = -1; }
                    }
                    draw();
                    return;
                }
            }
        }
        
        // Check for Ctrl+C interrupt
        if (interrupt_requested) {
            append_output_str("\nmultiWatch interrupted by Ctrl+C\n");
            // Kill all child processes
            for (int i=0;i<ncmd;i++) {
                if (pids[i] > 0) {
                    kill(pids[i], SIGINT);
                    waitpid(pids[i], NULL, 0);  // reap immediately
                    pids[i] = 0;
                }
            }
            // Close all pipes
            for (int i=0;i<ncmd;i++) {
                if (pfds[i].fd >= 0) {
                    close(pfds[i].fd);
                    pfds[i].fd = -1;
                }
            }
            draw();
            return;
        }
        
        int r = poll(pfds, (nfds_t)ncmd, 200);
        
        // Check interrupt again after poll (user may have pressed Ctrl+C during poll)
        if (interrupt_requested) {
            append_output_str("\nmultiWatch interrupted by Ctrl+C\n");
            for (int i=0;i<ncmd;i++) {
                if (pids[i] > 0) { kill(pids[i], SIGINT); waitpid(pids[i], NULL, 0); pids[i] = 0; }
            }
            for (int i=0;i<ncmd;i++) {
                if (pfds[i].fd >= 0) { close(pfds[i].fd); pfds[i].fd = -1; }
            }
            draw();
            return;
        }
        
        if (r>0) {
            for (int i=0;i<ncmd;i++) {
                if (pfds[i].fd < 0) continue;
                
                // Check interrupt before processing each stream
                if (interrupt_requested) {
                    append_output_str("\nmultiWatch interrupted by Ctrl+C\n");
                    for (int j=0;j<ncmd;j++) {
                        if (pids[j] > 0) { kill(pids[j], SIGINT); waitpid(pids[j], NULL, 0); pids[j] = 0; }
                    }
                    for (int j=0;j<ncmd;j++) {
                        if (pfds[j].fd >= 0) { close(pfds[j].fd); pfds[j].fd = -1; }
                    }
                    draw();
                    return;
                }
                
                if (pfds[i].revents & (POLLIN|POLLHUP)) {
                    char buf[256];
                    ssize_t n = read(pfds[i].fd, buf, sizeof(buf));
                    if (n > 0) {
                        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                        char tim[64];
                        snprintf(tim, sizeof(tim), "%ld.%03ld", (long)ts.tv_sec, ts.tv_nsec/1000000);
                        append_output_str("\n\""); append_output_str(cmds[i]); append_output_str("\" , ");
                        append_output_str(tim); append_output_str(":\n");
                        append_output_str("----------------------------------------------------\n");
                        append_output(buf, (size_t)n);
                        append_output_str("\n----------------------------------------------------\n");
                        draw();
                    }
                    if (n == 0 || (pfds[i].revents & POLLHUP)) { close(pfds[i].fd); pfds[i].fd = -1; open_streams--; draw(); }
                }
            }
        }
        for (int i=0;i<ncmd;i++) if (pids[i]>0) {
            int st; pid_t w = waitpid(pids[i], &st, WNOHANG);
            if (w==pids[i]) { pids[i]=0; }
        }
        // Handle Ctrl+C from GUI is done in main loop; we just stream here
    }
}
