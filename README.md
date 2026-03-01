# COMP 304 - Shell-ish (Spring 2026)

Repository: <https://github.com/yenikosebatuhan/Shell-ish>

This project implements an interactive Unix-style shell in C for COMP 304 Assignment 1.  
The implementation is based on the provided skeleton file and extended with required features for Parts I, II, and III.

---

## Build and Run

Compile the program:

gcc -o shell-ish shellish-skeleton.c

Run the shell:

./shell-ish

---

# Implemented Features

## Part I

- External command execution using `fork()` and `execv()`
- Manual PATH resolving (since `execv()` is used instead of `execvp()`)
- Background execution using `&`
- Built-in commands:
  - cd <path>
  - exit

---

## Part II

### I/O Redirection

Supported redirections:

- Input redirection:  
  command <file

- Output overwrite:  
  command >file

- Output append:  
  command >>file

IMPORTANT:

Redirection must be written WITHOUT space between symbol and file.

Correct examples:

echo hello >out.txt  
wc -l <in.txt  
echo test >>log.txt  

Not supported:

echo hello > out.txt  
wc -l < in.txt  

---

### Piping

Supports multi-stage pipelines:

ls -la | grep shell | wc -l  

Pipelines can also be combined with redirection.

---

## Part III – Built-in Commands

### 1) cut

A simplified implementation of the Unix cut command.

Default delimiter: TAB

Supported options:

- -d X
- --delimiter X
- -f list
- --fields list

Example usage:

cut -f1,3 <tab.txt  
cut -d ":" -f1,6 <colon.txt  
cat colon.txt | cut -d ":" -f1  

The command reads from standard input and prints selected fields in the specified order.

---

### 2) chatroom <roomname> <username>

A simple group chat system implemented using named pipes (FIFOs).

Room directory format:

/tmp/chatroom-<roomname>/

Each user has a FIFO file:

/tmp/chatroom-<roomname>/<username>

To exit the room, type:

/exit

Example:

chatroom comp304 ali  

Note:

chatroom is interactive and cannot be used inside a pipeline.

---

### 3) Custom Command – pinfo <pid>

Prints basic process information by reading:

/proc/<pid>/status

Displayed fields:

- Name
- State
- PPid
- VmSize
- VmRSS

Example usage:

pinfo 1  
pinfo 2  

Notes:

- Only numeric PID is supported.
- Invalid or non-existing PID prints an error message.
- pinfo $$ is not supported.

---

# Screenshots

All required screenshots for Parts I, II, and III are included in the imgs/ folder.

---

# Known Limitations

- Redirection requires no space between symbol and filename.
- chatroom cannot be used in a pipeline.
- pinfo only accepts numeric PIDs.
- This shell is a simplified educational implementation and does not fully replicate all behaviors of a real Unix shell.

---

# AI Usage Disclosure

According to the assignment rules, any use of AI must be disclosed.

In this project, an AI assistant (ChatGPT) was used only for:

- Planning test cases for screenshots (imgs folder)
- Writing and improving README content
- Adding simple explanatory comments to the code

All implementation logic, debugging, and final integration were completed by me.

---

# Author

Hüseyin Batuhan Yeniköse  
Koç University – Computer Engineering  
COMP 304 – Spring 2026# Shell-ish
