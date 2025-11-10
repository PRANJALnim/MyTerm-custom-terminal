#define _XOPEN_SOURCE 700
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <poll.h>
#include <sys/stat.h>
#include <errno.h>
#include "myterm.h"
#include "history.h"
#include "exec.h"
#include "multiwatch.h"

#define BUF_SIZE 8192
#define MAX_INPUT 1024
#define MAX_ARGS 128
#define MAX_PIPE 16
#define HISTORY_MAX 10000
#define MAX_TABS 8

static Display *dpy;
Display *dpy_global;  // exported for multiWatch event checking
static Window win;
static GC gc;
static int screen;
static XFontStruct *fontinfo = NULL;
typedef struct {
    char textbuf[BUF_SIZE];
    size_t textlen;
    char inputbuf[MAX_INPUT];
    size_t inputlen;
    size_t cursor_idx;
} Tab;

static Tab tabs[MAX_TABS];
static int active_tab = 0;

static char inputbuf[MAX_INPUT]; // legacy alias to active tab buffer for minimal edits
static size_t inputlen = 0;
static size_t cursor_idx = 0; // for Ctrl+A/E navigation
pid_t fg_child = -1;   // foreground child pid for signals
volatile int interrupt_requested = 0;  // set by Ctrl+C handler

// Background job tracking
BackgroundJob bg_jobs[MAX_JOBS];
int bg_job_count = 0;
static int line_height = 16;
static int win_width = 900, win_height = 600;
static int scroll_offset = 0; // number of lines scrolled up from bottom

static int search_mode = 0; // 0 normal, 1 waiting for search term
static char searchbuf[MAX_INPUT];
static size_t searchlen = 0;

// Tab completion state
static int completion_mode = 0;  // 0 normal, 1 waiting for selection
static char completion_matches[512][256];
static int completion_count = 0;
static size_t completion_token_start = 0;  // Where the token being completed starts

// forward declarations
void append_output(const char *s, size_t n);

// global capture state for the current foreground job (single at a time)
int cap_out_fd = -1;
int cap_err_fd = -1;
pid_t cap_pids[MAX_PIPE];
int cap_nstages = 0;
int cap_active = 0;
void append_output_str(const char *s);

static Colormap cmap;
static unsigned long col_bg, col_fg, col_tab_active, col_tab_inactive, col_accent;
static int tab_rect_x[MAX_TABS];
static int tab_rect_w[MAX_TABS];
static int tab_close_x[MAX_TABS];
static int tab_bar_h = 28;
static int newtab_x = 0;
static int newtab_w = 22;
static int tab_used[MAX_TABS];

void die(const char *msg) {
    perror(msg);
    exit(1);
}


void draw() {
    // Fill background
    XSetForeground(dpy, gc, col_bg);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)win_width, (unsigned)win_height);

    // Draw tab bar background
    XSetForeground(dpy, gc, col_tab_inactive);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)win_width, (unsigned)tab_bar_h);

    // Draw tabs with active styling and close buttons
    int x = 8; // baseline x for tab bar
    for (int i=0;i<MAX_TABS;i++) {
        if (!tab_used[i]) { tab_rect_x[i]=tab_rect_w[i]=tab_close_x[i]=0; continue; }
        char label[32]; snprintf(label, sizeof(label), "Tab %d%s", i+1, i==active_tab?"*":"");
        int tw = fontinfo ? XTextWidth(fontinfo, label, (int)strlen(label)) : (int)strlen(label)*8;
        int padx = 14; int closew = 14; int w = tw + padx*2 + closew + 6;
        int tx = x + padx; int ty = tab_bar_h - (tab_bar_h - (fontinfo?fontinfo->ascent:12)) / 2 - 6;
        tab_rect_x[i] = x; tab_rect_w[i] = w; tab_close_x[i] = x + w - closew - 8;
        // Tab background
        XSetForeground(dpy, gc, i==active_tab ? col_tab_active : col_tab_inactive);
        XFillRectangle(dpy, win, gc, x, 2, (unsigned)w, (unsigned)(tab_bar_h-4));
        // Tab label
        XSetForeground(dpy, gc, col_fg);
        XDrawString(dpy, win, gc, tx, ty, label, (int)strlen(label));
        // Close button box and X
        int cx = tab_close_x[i]; int cy = 6; int ch = tab_bar_h - 12;
        XSetForeground(dpy, gc, col_accent);
        XDrawRectangle(dpy, win, gc, cx, cy, closew, ch);
        XDrawLine(dpy, win, gc, cx+3, cy+3, cx+closew-3, cy+ch-3);
        XDrawLine(dpy, win, gc, cx+3, cy+ch-3, cx+closew-3, cy+3);
        x += w + 6;
    }
    // New tab "+" button at the end of tab bar
    newtab_x = x + 6; int cy = 6; int ch = tab_bar_h - 12;
    XSetForeground(dpy, gc, col_accent);
    XDrawRectangle(dpy, win, gc, newtab_x, cy, newtab_w, ch);
    // plus sign
    int cxm = newtab_x + newtab_w/2;
    int cym = cy + ch/2;
    XDrawLine(dpy, win, gc, cxm - 5, cym, cxm + 5, cym);
    XDrawLine(dpy, win, gc, cxm, cym - 5, cxm, cym + 5);
    // Bottom border of tab bar
    XSetForeground(dpy, gc, col_accent);
    XDrawLine(dpy, win, gc, 0, tab_bar_h, win_width, tab_bar_h);

    // Text area origin
    int text_origin_x = 10;
    int text_origin_y = tab_bar_h + 16;
    int xdraw = text_origin_x;
    int ydraw = text_origin_y;
    // Draw the accumulated text output with viewport/scrolling
    Tab *t = &tabs[active_tab];
    int available_lines = (win_height - ydraw) / line_height - 1; // keep one line for prompt
    if (available_lines < 0) available_lines = 0;
    // count total lines
    int total_lines = 0;
    for (size_t off=0; off < t->textlen; ) {
        char *nl = memchr(t->textbuf + off, '\n', t->textlen - off);
        total_lines++;
        if (!nl) break; else off = (size_t)((nl + 1) - t->textbuf);
    }
    if (t->textlen == 0) total_lines = 0;
    int max_offset = total_lines > available_lines ? (total_lines - available_lines) : 0;
    if (scroll_offset > max_offset) scroll_offset = max_offset;
    if (scroll_offset < 0) scroll_offset = 0;
    int start_line = total_lines - available_lines - scroll_offset;
    if (start_line < 0) start_line = 0;
    int drawn = 0;
    int current_line = 0;
    for (size_t off=0; off < t->textlen && drawn < available_lines; ) {
        char *line_start = t->textbuf + off;
        char *nl = memchr(line_start, '\n', t->textlen - off);
        size_t len = nl ? (size_t)(nl - line_start) : (t->textlen - off);
        if (current_line >= start_line) {
            if (len > 0) { XSetForeground(dpy, gc, col_fg); XDrawString(dpy, win, gc, xdraw, ydraw, line_start, (int)len); }
            ydraw += line_height;
            drawn++;
        }
        current_line++;
        if (!nl) break; else off = (size_t)((nl + 1) - t->textbuf);
    }
    // Draw prompt and current input (use current working directory)
    char prompt[512];
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd)) == NULL) snprintf(cwd, sizeof(cwd), "?");
    // Safely build prompt: "<cwd> " ensuring no overflow
    size_t l = strlen(cwd);
    if (l > sizeof(prompt) - 20) l = sizeof(prompt) - 20; // room for indicator + "> " and NUL
    memcpy(prompt, cwd, l);
    // Add running indicator if command is active
    if (cap_active) {
        memcpy(prompt + l, " [running]", 10);
        l += 10;
    }
    prompt[l] = '>';
    prompt[l+1] = ' ';
    prompt[l+2] = '\0';
    XSetForeground(dpy, gc, col_fg);
    XDrawString(dpy, win, gc, xdraw, ydraw, prompt, (int)strlen(prompt));
    int ix = xdraw + (fontinfo ? XTextWidth(fontinfo, prompt, (int)strlen(prompt)) : 8 * (int)strlen(prompt));
    // Draw multi-line input after the prompt: first line continues from ix, subsequent lines from xdraw
    int line_index = 0; int caret_line = 0; int caret_col = 0;
    // Compute caret line/col
    for (size_t i=0;i<=t->cursor_idx && i<=t->inputlen; i++) {
        if (i==t->cursor_idx) { break; }
        if (t->inputbuf[i]=='\n') { caret_line++; caret_col=0; }
        else { caret_col++; }
    }
    // Draw input lines
    size_t off = 0;
    while (off < t->inputlen) {
        char *nl = memchr(t->inputbuf + off, '\n', t->inputlen - off);
        size_t len = nl ? (size_t)(nl - (t->inputbuf + off)) : (t->inputlen - off);
        if (line_index == 0) {
            if (len > 0) XDrawString(dpy, win, gc, ix, ydraw, t->inputbuf + off, (int)len);
        } else {
            if (len > 0) XDrawString(dpy, win, gc, xdraw, ydraw, t->inputbuf + off, (int)len);
        }
        ydraw += line_height; line_index++;
        if (!nl) break; else off = (size_t)((nl + 1) - t->inputbuf);
        // draw empty line if consecutive '\n'
        if (off <= t->inputlen && (nl && (off == (size_t)((nl + 1) - t->inputbuf)))) {
            // no-op; handled by loop naturally
        }
    }
    // Caret position based on caret_line/col
    int caret_x;
    // Base Y where the caret line sits: if no input lines drawn, stay on prompt baseline
    int caret_base_y = (line_index == 0) ? ydraw : (ydraw - line_height);
    if (caret_line == 0) {
        if (fontinfo) caret_x = ix + XTextWidth(fontinfo, t->inputbuf, (int)caret_col);
        else caret_x = ix + 8 * caret_col;
        int cy = caret_base_y;
        XSetForeground(dpy, gc, col_accent);
        XDrawLine(dpy, win, gc, caret_x, cy + 2, caret_x, cy - line_height + 4);
    } else {
        // compute x from start for the segment of current line
        size_t start = 0; int l = caret_line;
        for (size_t i=0;i<t->cursor_idx && l>0; i++) { if (t->inputbuf[i]=='\n') { start = i+1; l--; } }
        if (fontinfo) caret_x = xdraw + XTextWidth(fontinfo, t->inputbuf + start, (int)caret_col);
        else caret_x = xdraw + 8 * caret_col;
        int cy = caret_base_y;
        XSetForeground(dpy, gc, col_accent);
        XDrawLine(dpy, win, gc, caret_x, cy + 2, caret_x, cy - line_height + 4);
    }
    XFlush(dpy);
}

void append_output(const char *s, size_t n) {
    if (n == 0) return;
    Tab *t = &tabs[active_tab];
    if (n > BUF_SIZE - t->textlen) n = BUF_SIZE - t->textlen;
    memcpy(t->textbuf + t->textlen, s, n);
    t->textlen += n;
    // if at bottom (scroll_offset==0), remain at bottom as new output arrives
}

void append_output_str(const char *s) {
    append_output(s, strlen(s));
}

// nonblocking pump of child output and child exit reaping
static void pump_child_io() {
    int progress = 0;
    char buf[1024];
    // read stdout
    if (cap_out_fd != -1) {
        for (;;) {
            ssize_t r = read(cap_out_fd, buf, sizeof(buf));
            if (r > 0) { append_output(buf, (size_t)r); progress = 1; }
            else if (r == 0) { close(cap_out_fd); cap_out_fd = -1; break; }
            else { if (errno == EAGAIN || errno == EWOULDBLOCK) break; else { close(cap_out_fd); cap_out_fd=-1; break; } }
        }
    }
    // read stderr
    if (cap_err_fd != -1) {
        for (;;) {
            ssize_t r = read(cap_err_fd, buf, sizeof(buf));
            if (r > 0) { append_output(buf, (size_t)r); progress = 1; }
            else if (r == 0) { close(cap_err_fd); cap_err_fd = -1; break; }
            else { if (errno == EAGAIN || errno == EWOULDBLOCK) break; else { close(cap_err_fd); cap_err_fd=-1; break; } }
        }
    }
    // reap children non-blocking
    if (cap_active) {
        int alive = 0;
        for (int i=0;i<cap_nstages;i++) if (cap_pids[i] > 0) {
            int st; pid_t w = waitpid(cap_pids[i], &st, WNOHANG);
            if (w == 0) alive = 1; else if (w == cap_pids[i]) cap_pids[i] = 0; else alive = 1;
        }
        if (!alive && cap_out_fd == -1 && cap_err_fd == -1) {
            cap_active = 0; fg_child = -1;
            // Show completion message for commands that produce no output
            progress = 1;  // Force redraw to show prompt
        }
    }
    if (progress) draw();
}

/* history functions moved to src/history.c */

/* print_history_command moved to src/history.c */

/* parser helpers now in src/exec.c */

void clear_screen() {
    Tab *t = &tabs[active_tab];
    t->textlen = 0;
    t->textbuf[0] = '\0';
}

static int is_whitespace(char c) { return c==' '||c=='\t' || c=='\n'; }

// Replace embedded newlines with spaces and trim/collapse whitespace so multiline input
// executes as a single command line.
static void normalize_command(char *s) {
    // map CR/LF to space
    for (char *p = s; *p; ++p) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }
    // trim leading/trailing spaces and collapse multiple spaces
    size_t r = 0, w = 0; int in_space = 0;
    // skip leading spaces
    while (s[r] && (s[r] == ' ' || s[r] == '\t')) r++;
    for (; s[r]; ++r) {
        if (s[r] == ' ' || s[r] == '\t') {
            if (!in_space) { s[w++] = ' '; in_space = 1; }
        } else {
            s[w++] = s[r]; in_space = 0;
        }
    }
    // trim trailing space
    if (w>0 && s[w-1] == ' ') w--;
    s[w] = '\0';
}

/* parse_args moved to src/exec.c */

/* split_pipes moved to src/exec.c */

/* execute_pipeline moved to src/exec.c */

static void handle_ctrl_c() {
    // Set interrupt flag for blocking operations (e.g., multiWatch)
    interrupt_requested = 1;
    
    if (cap_active) {
        // Kill all processes in the pipeline
        for (int i = 0; i < cap_nstages; i++) {
            if (cap_pids[i] > 0) {
                kill(cap_pids[i], SIGINT);
                cap_pids[i] = 0;
            }
        }
        // Close capture pipes
        if (cap_out_fd != -1) { close(cap_out_fd); cap_out_fd = -1; }
        if (cap_err_fd != -1) { close(cap_err_fd); cap_err_fd = -1; }
        // Reset capture state
        cap_active = 0;
        fg_child = -1;
        append_output_str("^C\n");
        draw();
    } else {
        // No running command: clear input line and show ^C
        Tab *t = &tabs[active_tab];
        if (t->inputlen > 0) {
            append_output_str("^C\n");
            t->inputlen = 0;
            t->cursor_idx = 0;
            t->inputbuf[0] = '\0';
            draw();
        }
    }
}

static void handle_ctrl_z() {
    if (cap_active) {
        // Find a free job slot
        int job_idx = -1;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (!bg_jobs[i].active) {
                job_idx = i;
                break;
            }
        }
        
        if (job_idx == -1) {
            append_output_str("^Z\n[Too many background jobs]\n");
            // Kill the process instead
            for (int i = 0; i < cap_nstages; i++) {
                if (cap_pids[i] > 0) kill(cap_pids[i], SIGKILL);
            }
        } else {
            // Store job information
            bg_jobs[job_idx].active = 1;
            bg_jobs[job_idx].nprocs = cap_nstages;
            for (int i = 0; i < cap_nstages; i++) {
                bg_jobs[job_idx].pids[i] = cap_pids[i];
            }
            // Store command (get from last input)
            Tab *t = &tabs[active_tab];
            if (t->inputlen > 0 && t->inputlen < sizeof(bg_jobs[job_idx].command)) {
                memcpy(bg_jobs[job_idx].command, t->inputbuf, t->inputlen);
                bg_jobs[job_idx].command[t->inputlen] = '\0';
            } else {
                snprintf(bg_jobs[job_idx].command, sizeof(bg_jobs[job_idx].command), "(unknown)");
            }
            
            bg_job_count++;
            
            // Send SIGCONT to resume processes in background
            // (They were never stopped, so this just ensures they continue)
            for (int i = 0; i < cap_nstages; i++) {
                if (cap_pids[i] > 0) {
                    kill(cap_pids[i], SIGCONT);
                }
            }
            
            char msg[128];
            snprintf(msg, sizeof(msg), "^Z\n[%d] %d\n", job_idx + 1, bg_jobs[job_idx].pids[cap_nstages-1]);
            append_output_str(msg);
        }
        
        // Close capture pipes (detach from process output)
        if (cap_out_fd != -1) { close(cap_out_fd); cap_out_fd = -1; }
        if (cap_err_fd != -1) { close(cap_err_fd); cap_err_fd = -1; }
        // Reset capture state (process moves to background)
        cap_active = 0;
        fg_child = -1;
        draw();
    } else {
        // No running command
        Tab *t = &tabs[active_tab];
        if (t->inputlen > 0) {
            append_output_str("^Z\n");
            draw();
        }
    }
}

static void complete_tab() {
    Tab *t = &tabs[active_tab];
    
    // Find current token before cursor
    size_t start = t->cursor_idx;
    while (start > 0 && t->inputbuf[start-1] != ' ' && t->inputbuf[start-1] != '\t') {
        start--;
    }
    
    char prefix[256] = "";
    size_t plen = t->cursor_idx - start;
    if (plen >= sizeof(prefix)) return;
    memcpy(prefix, t->inputbuf + start, plen);
    prefix[plen] = '\0';
    
    // If prefix is empty, don't complete
    if (plen == 0) return;
    
    // Scan directory for matches
    DIR *d = opendir(".");
    if (!d) return;
    
    char matches[512][256];
    int mcount = 0;
    struct dirent *de;
    
    while ((de = readdir(d)) != NULL) {
        // Skip . and ..
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        
        if (strncmp(de->d_name, prefix, plen) == 0) {
            if (mcount < 512) {
                snprintf(matches[mcount], sizeof(matches[mcount]), "%s", de->d_name);
                mcount++;
            }
        }
    }
    closedir(d);
    
    // No matches - do nothing
    if (mcount == 0) {
        return;
    }
    
    // Single match - complete fully
    if (mcount == 1) {
        size_t add = strlen(matches[0]) - plen;
        if (t->inputlen + add < MAX_INPUT - 1) {
            // Insert the rest of the filename
            memmove(t->inputbuf + t->cursor_idx + add, 
                    t->inputbuf + t->cursor_idx, 
                    t->inputlen - t->cursor_idx);
            memcpy(t->inputbuf + start + plen, matches[0] + plen, add);
            t->inputlen += add;
            t->cursor_idx += add;
            t->inputbuf[t->inputlen] = '\0';
        }
        return;
    }
    
    // Multiple matches - find longest common prefix
    char lcp[256];
    strncpy(lcp, matches[0], sizeof(lcp));
    lcp[sizeof(lcp)-1] = '\0';
    
    for (int i = 1; i < mcount; i++) {
        int j = 0;
        while (lcp[j] && matches[i][j] && lcp[j] == matches[i][j]) {
            j++;
        }
        lcp[j] = '\0';
    }
    
    // If longest common prefix is longer than current prefix, complete to it
    if (strlen(lcp) > plen) {
        size_t add = strlen(lcp) - plen;
        if (t->inputlen + add < MAX_INPUT - 1) {
            memmove(t->inputbuf + t->cursor_idx + add,
                    t->inputbuf + t->cursor_idx,
                    t->inputlen - t->cursor_idx);
            memcpy(t->inputbuf + start + plen, lcp + plen, add);
            t->inputlen += add;
            t->cursor_idx += add;
            t->inputbuf[t->inputlen] = '\0';
        }
        // After completing to LCP, check if still multiple matches
        // If yes, show numbered list
        if (mcount > 1) {
            append_output_str("\nMultiple matches:\n");
            for (int i = 0; i < mcount; i++) {
                char num[32];
                snprintf(num, sizeof(num), "%d. %s\n", i + 1, matches[i]);
                append_output_str(num);
            }
            append_output_str("Enter number to select: ");
            
            // Save matches for selection
            completion_mode = 1;
            completion_count = mcount;
            completion_token_start = start;
            for (int i = 0; i < mcount; i++) {
                strncpy(completion_matches[i], matches[i], sizeof(completion_matches[i]));
            }
        }
    } else {
        // LCP is same as prefix, show numbered list immediately
        append_output_str("\nMultiple matches:\n");
        for (int i = 0; i < mcount; i++) {
            char num[32];
            snprintf(num, sizeof(num), "%d. %s\n", i + 1, matches[i]);
            append_output_str(num);
        }
        append_output_str("Enter number to select: ");
        
        // Save matches for selection
        completion_mode = 1;
        completion_count = mcount;
        completion_token_start = start;
        for (int i = 0; i < mcount; i++) {
            strncpy(completion_matches[i], matches[i], sizeof(completion_matches[i]));
        }
    }
}

static void run_command(char *cmdline) {
    if (cmdline == NULL || *cmdline == '\0') return;

    // Clear interrupt flag from previous Ctrl+C
    interrupt_requested = 0;

    // background?
    int background = 0;
    // normalize multiline to single line
    normalize_command(cmdline);
    size_t L = strlen(cmdline);
    while (L>0 && is_whitespace(cmdline[L-1])) cmdline[--L]='\0';
    if (L>0 && cmdline[L-1]=='&') { background=1; cmdline[L-1]='\0'; }

    // multiWatch?
    if (strncmp(cmdline, "multiWatch", 10)==0) {
        char *p = cmdline+10; while (is_whitespace(*p)) p++;
        multiwatch_run(p);
        return;
    }

    execute_pipeline(cmdline, background);
}

int main() {
    setlocale(LC_ALL, "");
    dpy = XOpenDisplay(NULL);
    dpy_global = dpy;  // export for multiWatch
    if (!dpy) die("XOpenDisplay");
    screen = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 100, 100, 900, 600, 1,
                              BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XStoreName(dpy, win, "MyTerm");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    XMapWindow(dpy, win);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));
    // Colors
    cmap = DefaultColormap(dpy, screen);
    XColor scr, exact;
    if (XAllocNamedColor(dpy, cmap, "#121212", &scr, &exact)) col_bg = scr.pixel; else col_bg = BlackPixel(dpy, screen);
    if (XAllocNamedColor(dpy, cmap, "#E6EDF3", &scr, &exact)) col_fg = scr.pixel; else col_fg = WhitePixel(dpy, screen);
    if (XAllocNamedColor(dpy, cmap, "#1F2937", &scr, &exact)) col_tab_active = scr.pixel; else col_tab_active = col_bg;
    if (XAllocNamedColor(dpy, cmap, "#0B0F14", &scr, &exact)) col_tab_inactive = scr.pixel; else col_tab_inactive = col_bg;
    if (XAllocNamedColor(dpy, cmap, "#10B981", &scr, &exact)) col_accent = scr.pixel; else col_accent = col_fg;
    // Load a fixed-width font for proper caret placement
    fontinfo = XLoadQueryFont(dpy, "fixed");
    if (fontinfo) {
        XSetFont(dpy, gc, fontinfo->fid);
        line_height = fontinfo->ascent + fontinfo->descent + 2;
    }

    // init tabs
    for (int i=0;i<MAX_TABS;i++){ tabs[i].textlen=0; tabs[i].inputlen=0; tabs[i].cursor_idx=0; tab_used[i]=0; }
    tab_used[0] = 1; // show Tab 1 by default
    active_tab = 0;
    // history
    history_init();
    history_load();
    append_output_str("Welcome to MyTerm\n");

    for (;;) {
        // 1) Pump child IO so output continues streaming
        pump_child_io();

        // 2) Handle all pending X events without blocking
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose) {
                draw();
            } else if (ev.type == ConfigureNotify) {
                win_width = ev.xconfigure.width; win_height = ev.xconfigure.height; draw();
            } else if (ev.type == KeyPress) {
                KeySym keysym;
                char buf[32];
                int len = XLookupString(&ev.xkey, buf, sizeof(buf), &keysym, NULL);
                Tab *t = &tabs[active_tab];

                // Handle completion mode (number selection)
                if (completion_mode) {
                    if (keysym == XK_Return || keysym == XK_Escape) {
                        // Cancel completion
                        completion_mode = 0;
                        append_output_str("\n");
                        draw();
                    } else if (len > 0 && buf[0] >= '1' && buf[0] <= '9') {
                        int selection = buf[0] - '0';
                        if (selection > 0 && selection <= completion_count) {
                            // Replace token with selected file
                            char *selected = completion_matches[selection - 1];
                            
                            // Find end of current token
                            size_t end = completion_token_start;
                            while (end < t->inputlen && t->inputbuf[end] != ' ' && t->inputbuf[end] != '\t') {
                                end++;
                            }
                            
                            // Calculate new length
                            size_t token_len = end - completion_token_start;
                            size_t new_len = strlen(selected);
                            
                            if (t->inputlen - token_len + new_len < MAX_INPUT - 1) {
                                // Remove old token and insert new one
                                memmove(t->inputbuf + completion_token_start + new_len,
                                        t->inputbuf + end,
                                        t->inputlen - end);
                                memcpy(t->inputbuf + completion_token_start, selected, new_len);
                                t->inputlen = t->inputlen - token_len + new_len;
                                t->cursor_idx = completion_token_start + new_len;
                                t->inputbuf[t->inputlen] = '\0';
                                
                                char msg[300];
                                // Truncate filename if too long to fit in message buffer
                                char safe_name[256];
                                strncpy(safe_name, selected, sizeof(safe_name) - 1);
                                safe_name[sizeof(safe_name) - 1] = '\0';
                                snprintf(msg, sizeof(msg), "\nSelected: %s\n", safe_name);
                                append_output_str(msg);
                            }
                        } else {
                            append_output_str("\nInvalid selection\n");
                        }
                        completion_mode = 0;
                        draw();
                    }
                    continue;
                }
                
                if (search_mode) {
                    if (keysym == XK_Return) {
                        searchbuf[searchlen]='\0';
                        append_output_str("Search: "); append_output_str(searchbuf); append_output_str("\n");
                        history_search_and_print(searchbuf);
                        searchlen=0; searchbuf[0]='\0'; search_mode=0; draw();
                    } else if (keysym == XK_BackSpace) {
                        if (searchlen>0) { searchlen--; searchbuf[searchlen]='\0'; }
                        draw();
                    } else if (len>0 && searchlen+len<MAX_INPUT-1) {
                        memcpy(searchbuf+searchlen, buf, (size_t)len); searchlen+=len; searchbuf[searchlen]='\0';
                        draw();
                    }
                    continue;
                }

                if ((ev.xkey.state & ControlMask) && (keysym == XK_c || keysym==XK_C)) {
                    handle_ctrl_c();
                    draw();
                } else if ((ev.xkey.state & ControlMask) && (keysym == XK_z || keysym==XK_Z)) {
                    handle_ctrl_z();
                    draw();
                } else if ((ev.xkey.state & ControlMask) && (keysym == XK_t || keysym==XK_T)) {
                    // new tab (Ctrl+T)
                    int nt = -1;
                    for (int i=0;i<MAX_TABS;i++) if (!tab_used[i]) { nt=i; break; }
                    if (nt != -1) {
                        tabs[nt].textlen=0; tabs[nt].inputlen=0; tabs[nt].cursor_idx=0; tab_used[nt]=1; active_tab=nt;
                    }
                    draw();
                } else if ((ev.xkey.state & ControlMask) && (keysym == XK_R || keysym == XK_r)) {
                    // Ctrl+R search
                    search_mode = 1; searchlen=0; searchbuf[0]='\0'; append_output_str("Enter search term: "); draw();
                } else if ((ev.xkey.state & ControlMask) && keysym == XK_a) {
                    t->cursor_idx = 0; draw();
                } else if ((ev.xkey.state & ControlMask) && keysym == XK_e) {
                    t->cursor_idx = t->inputlen; draw();
                } else if (keysym == XK_Tab) {
                    complete_tab(); draw();
                } else if (keysym == XK_Left) {
                    if (t->cursor_idx>0) {
                        t->cursor_idx--;
                    }
                    draw();
                } else if (keysym == XK_Right) {
                    if (t->cursor_idx<t->inputlen) {
                        t->cursor_idx++;
                    }
                    draw();
                } else if (keysym == XK_Prior && !(ev.xkey.state & ControlMask)) {
                    scroll_offset += 3; draw();
                } else if (keysym == XK_Next && !(ev.xkey.state & ControlMask)) {
                    scroll_offset -= 3; if (scroll_offset < 0) scroll_offset = 0; draw();
                } else if ((ev.xkey.state & ControlMask) && keysym == XK_Prior) { // Ctrl+PageUp -> prev tab
                    for (int k=1;k<=MAX_TABS;k++) { int j=(active_tab - k + MAX_TABS)%MAX_TABS; if (tab_used[j]) { active_tab=j; break; } }
                    draw();
                } else if ((ev.xkey.state & ControlMask) && keysym == XK_Next) { // Ctrl+PageDown -> next tab
                    for (int k=1;k<=MAX_TABS;k++) { int j=(active_tab + k)%MAX_TABS; if (tab_used[j]) { active_tab=j; break; } }
                    draw();
                } else if (keysym == XK_Return || keysym == XK_KP_Enter || (len>0 && (buf[0]=='\r' || buf[0]=='\n'))) {
                    // Shift+Enter inserts a newline instead of executing
                    if (ev.xkey.state & ShiftMask) {
                        if (t->inputlen + 1 < MAX_INPUT-1) {
                            memmove(t->inputbuf + t->cursor_idx + 1, t->inputbuf + t->cursor_idx, t->inputlen - t->cursor_idx);
                            t->inputbuf[t->cursor_idx] = '\n';
                            t->inputlen++; t->cursor_idx++;
                            t->inputbuf[t->inputlen] = '\0';
                        }
                        draw();
                        continue;
                    }
                    t->inputbuf[t->inputlen] = '\0';
                    // Echo the prompt and command into the scrollback so it remains visible
                    char cwd[512]; if (getcwd(cwd, sizeof(cwd)) == NULL) snprintf(cwd, sizeof(cwd), "?");
                    append_output_str(cwd); append_output_str("> "); append_output_str(t->inputbuf); append_output_str("\n");
                    draw();
                    if (t->inputlen > 0) {
                        add_history(t->inputbuf);
                        history_save_append(t->inputbuf);
                        memcpy(inputbuf, t->inputbuf, t->inputlen+1);
                        inputlen = t->inputlen; cursor_idx = t->cursor_idx;
                        run_command(t->inputbuf);
                        scroll_offset = 0;
                    }
                    t->inputlen = 0; t->inputbuf[0] = '\0'; t->cursor_idx = 0; draw();
                } else if (keysym == XK_BackSpace) {
                    if (t->cursor_idx>0) {
                        memmove(t->inputbuf+t->cursor_idx-1, t->inputbuf+t->cursor_idx, t->inputlen-t->cursor_idx);
                        t->inputlen--; t->cursor_idx--; t->inputbuf[t->inputlen] = '\0';
                    }
                    draw();
                } else if (len > 0) {
                    if (t->inputlen + len < MAX_INPUT-1) {
                        memmove(t->inputbuf+t->cursor_idx+len, t->inputbuf+t->cursor_idx, t->inputlen-t->cursor_idx);
                        memcpy(t->inputbuf + t->cursor_idx, buf, (size_t)len);
                        t->inputlen += (size_t)len; t->cursor_idx += (size_t)len; t->inputbuf[t->inputlen] = '\0';
                    }
                    draw();
                }
            } else if (ev.type == ButtonPress) {
                int mx = ev.xbutton.x;
                int my = ev.xbutton.y;
                // Tab clicks (switch/close)
                if (my >= 0 && my <= tab_bar_h) {
                    // New tab button
                    if (mx >= newtab_x && mx <= newtab_x + newtab_w) {
                        int nt = -1;
                        for (int i=0;i<MAX_TABS;i++) if (!tab_used[i]) { nt=i; break; }
                        if (nt != -1) {
                            tabs[nt].textlen=0; tabs[nt].inputlen=0; tabs[nt].cursor_idx=0; tab_used[nt]=1; active_tab=nt;
                        }
                        draw();
                        continue;
                    }
                    for (int i=0;i<MAX_TABS;i++) {
                        int rx = tab_rect_x[i], rw = tab_rect_w[i];
                        if (rw <= 0) continue;
                        if (mx >= rx && mx <= rx+rw) {
                            // Check close box
                            int cx = tab_close_x[i]; int closew = 14; int cy = 6; int ch = tab_bar_h - 12;
                            if (mx >= cx && mx <= cx+closew && my >= cy && my <= cy+ch) {
                                // close tab
                                tabs[i].textlen = 0; tabs[i].inputlen = 0; tabs[i].cursor_idx = 0; tab_used[i]=0;
                                if (active_tab == i) {
                                    int nt = -1; for (int k=0;k<MAX_TABS;k++) if (tab_used[k]) { nt=k; break; }
                                    if (nt == -1) { tab_used[0]=1; nt=0; }
                                    active_tab = nt;
                                }
                                draw();
                            } else {
                                // activate tab
                                if (tab_used[i]) { active_tab = i; draw(); }
                            }
                            break;
                        }
                    }
                } else {
                    // Scroll wheel handling in content area
                    if (ev.xbutton.button == Button4) { scroll_offset += 3; draw(); }
                    else if (ev.xbutton.button == Button5) { scroll_offset -= 3; if (scroll_offset < 0) scroll_offset = 0; draw(); }
                }
            }
        }

        // 3) Small sleep to avoid busy spin
        struct timespec ts = {0, 16 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    return 0;
}
