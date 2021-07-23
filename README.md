# Linux Mini Shell
A simple Linux OS shell in C

### Compilation
From command line: `gcc -o minish minish.c` 

### Operation
From command line: `minish` (changes to file permissions may be required to allow execution) \
Type `exit` to quit at any time

Built-in commands:
- `exit`  exits program, killing all other processes/jobs
- `status`  retrieves most recent exit status or terminating signal
- `cd`  changes directory to home directory
- `cd <dir>`  changes directory to specified directory

Executes other Linux commands (`ls`, `pwd`, etc.) utilizing default behavior via `fork()` & `exec()` \
Supported features:
- redirection with use of `>` and/or `<` operators
- execution of background jobs with use of `&` symbol as last command line argument (preceded by space)
- expansion of all instances of `$$` to PID
- `<CTRL>-Z` (SIGTSTP) to toggle foreground-only mode on/off (prevents running of background jobs)
- `<CTRL>-C` (SIGINT) to terminate foreground processes

Does _not_ support globbing, quoting (multi-word arguments with spaces), or use of pipe operator (`|`)
