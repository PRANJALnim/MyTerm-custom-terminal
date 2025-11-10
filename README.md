# MyTerm

MyTerm is a feature-rich terminal emulator built from scratch using X11 (Xlib) for the GUI. It provides a modern shell experience with support for tabs, pipes, I/O redirection, command history, and features like parallel command execution.


## Features

### **X11 Graphical Interface**
- Custom GUI built with Xlib 
- Multiple independent tabs (Ctrl+T to create, click to switch)
- scrolling with PageUp/PageDown and mouse wheel
- Visual tab bar with close buttons
- Real-time output rendering

### **Core Shell Functionality**
- Execute external commands (`ls`, `gcc`, `./program`, etc.)
- Change directories with `cd`
- Run programs with arguments
- Multiline command input (Shift+Enter)

### **I/O Redirection**
- **Input redirection**: `command < input.txt`
- **Output redirection**: `command > output.txt`
- **Combined**: `./program < input.txt > output.txt`

### **Pipes**
- Single pipes: `ls | wc -l`
- Multi-stage pipelines: `cat file.txt | grep pattern | sort | uniq`

### **multiWatch : Parallel Command Execution**
Run multiple commands in parallel and see their outputs interleaved with timestamps:
```bash
multiWatch ["command1", "command2", "command3"]
```
Example:
```bash
multiWatch ["for i in 1 2 3; do echo A$i; sleep 0.5; done", "for i in 1 2 3; do echo B$i; sleep 0.5; done"]
```
Output shows each command's results with Unix timestamps as they arrive.

### **Command History**
- Stores last 10,000 commands persistently in `~/.myterm_history`
- View history: `history` (shows last 1000 entries)
- Search history: Press **Ctrl+R**, type search term, press Enter
- Fuzzy matching when exact match not found

###  **Keyboard Shortcuts**
- **Ctrl+A**: Move cursor to start of line
- **Ctrl+E**: Move cursor to end of line
- **Ctrl+C**: Interrupt running command
- **Ctrl+Z**: Suspend command to background
- **Ctrl+R**: Search command history
- **Ctrl+T**: Create new tab
- **Tab**: Auto-complete file names
- **Shift+Enter**: Insert newline (multiline input)

### **Tab Completion**
- Press **Tab** after typing partial filename
- Completes to longest common prefix if multiple matches
- Shows all matching files if ambiguous

### **Signal Handling**
- **Ctrl+C**: Sends SIGINT to foreground process (doesn't exit shell)
- **Ctrl+Z**: Sends SIGTSTP to suspend process
- Works with pipelines and multiWatch

---

## Installation

### Prerequisites
```bash
# Ubuntu
sudo apt-get install gcc make libx11-dev


### Build
```bash
cd /path/to/myterm_25CS60R39
make
```

This compiles all source files and creates the `myterm` executable.

### Clean Build
```bash
make        # Rebuild from scratch
```

---

## Usage

### Starting MyTerm
```bash
./myterm
```

Or use the Makefile shortcut:
```bash
make run
```

### Basic Commands

**Navigate directories:**
```bash
cd /tmp
pwd
ls -la
```

**Run programs:**
```bash
gcc -o myprogram myprogram.c
./myprogram
```

**Pipe commands:**
```bash
ls | grep .txt
ps aux | grep myterm | wc -l
```

**Redirect I/O:**
```bash
echo "Hello World" > output.txt
sort < unsorted.txt > sorted.txt
cat file1.txt file2.txt >> combined.txt
```

**Background jobs:**
```bash
sleep 30 &
echo "This runs immediately"
```

### Advanced Features

**Parallel execution with multiWatch:**
```bash
multiWatch ["for i in 1 2 3 4 5; do echo $i; sleep 1; done", "for i in 6 7 8 9 10; do echo $i; sleep 1; done"]
```

**Search command history:**
1. Press **Ctrl+R**
2. Type search term (e.g., `grep`)
3. Press **Enter** to see matching commands

**Tab completion:**
```bash
cat myf<Tab>  # Completes to myfile.txt if unique
```

**Multiline commands:**
```bash
echo "Line 1" && <Shift+Enter>
echo "Line 2" && <Shift+Enter>
echo "Line 3" <Enter>
```

**Multiple tabs:**
- Press **Ctrl+T** to open new tab
- Click tab labels to switch
- Click **X** button to close tab
- Each tab has independent command history and output

---

## Command Reference

### Built-in Commands

| Command | Description | Example |
|---------|-------------|----------|
| `cd` | Change directory | `cd /tmp` |
| `clear` | Clear the screen | `clear` |
| `history` | Show last 1000 commands | `history` |
| `jobs` | Show background jobs | `jobs` |
| `help` | Show help message | `help` |
| `multiWatch` | Run commands in parallel | `multiWatch ["cmd1", "cmd2"]` |

### Redirection Operators

| Operator | Description | Example |
|----------|-------------|----------|
| `<` | Input from file | `sort < data.txt` |
| `>` | Output to file (overwrite) | `ls > files.txt` |
| `>>` | Append to file | `echo "log" >> log.txt` |
| `|` | Pipe to next command | `cat file | grep pattern` |
| `&` | Run in background | `sleep 10 &` |

### Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+A | Move to line start |
| Ctrl+E | Move to line end |
| Ctrl+C | Interrupt command |
| Ctrl+Z | Suspend to background |
| Ctrl+R | Search history |
| Ctrl+T | New tab |
| Ctrl+PageUp | Previous tab |
| Ctrl+PageDown | Next tab |
| Tab | File name completion |
| Shift+Enter | Insert newline |
| PageUp/PageDown | Scroll output |

---

## Architecture

### Module Structure

```
src/
├── main.c         # X11 GUI, event loop, tab management
├── exec.c/h       # Command execution, pipes, redirection
├── history.c/h    # Command history, search
├── multiwatch.c/h # Parallel command execution
└── myterm.h       # Shared declarations
```

### Key Technologies
- **X11 (Xlib)**: Window management, event handling, rendering
- **POSIX APIs**: `fork()`, `execvp()`, `pipe()`, `dup2()`
- **poll()**: Async I/O for multiWatch
- **Signals**: SIGINT, SIGTSTP handling

---


## Project Structure

```
myterm_25CS60R39/
├── src/                  # Source code
│   ├── main.c           # Main program
│   ├── exec.c/h         # Execution engine
│   ├── history.c/h      # History management
│   ├── multiwatch.c/h   # Parallel execution
│   └── myterm.h         # Common header
├── Makefile             # Build configuration
├── README.md            # This file
├── DESIGNDOC.md         # Architecture details
```

---

## Author

- Name: Pranjal Nimbodiya

---
