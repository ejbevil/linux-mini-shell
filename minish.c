/* **************************************
Erica Bevilacqua (bevilace)
minish.c
*************************************** */

#include <errno.h>
#include <fcntl.h>
#include <signal.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define _GNU_SOURCE
#define INPMAX 2048   // max command line chars, incl '\n'  (per spec)
#define ARGMAX 512    // max num command line args  (per spec)
#define PROCMAX 100   // max num processes  (arbitrary)

volatile sig_atomic_t fgOnlyMode = false;   // global var to allow use by signal handler functions

void CatchSIGTSTP(int);
void ReadInput(char*, int);
bool IsLineToIgnore(char*);
int  ParseInput(char*, char* []);
void Expand$$(char*);
void SubstrReplace(char*, char*, char*, int);
void ExitCleanup(pid_t []);
void ChangeDirectory(char*);
void DisplayStatus(int);
int  FindRedirFilepath(char* [], char*);
bool IsBGCommand(char* [], int);
void RemoveArgs(char* [], int, int, int*);
void ExecuteCommand (char**);
void AddToBGProcList(pid_t [], pid_t, int*);
void RemFromBGProcList(pid_t [], pid_t, int*);
void ReapZombies(pid_t [], int*);

int main() {
    char  userInput[INPMAX];                        // buffer for user input
    int   childExitMethod = -5;                     // child exit method
    int   numBGProcs = 0;                           // num background proc's
    pid_t bgPidList[PROCMAX];                       // array of background PIDs
    memset(bgPidList, -1, PROCMAX * sizeof(int));

    // signal setup
    struct sigaction SIGTSTP_action, ignore_action, default_action;
    memset(&SIGTSTP_action, 0, sizeof(struct sigaction));
    memset(&ignore_action, 0, sizeof(struct sigaction));
    memset(&default_action, 0, sizeof(struct sigaction));    
    ignore_action.sa_handler = SIG_IGN;         // general ignore
    default_action.sa_handler = SIG_DFL;        // general default
    SIGTSTP_action.sa_handler = CatchSIGTSTP;
    sigfillset(&SIGTSTP_action.sa_mask);  
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);  // catch to toggle foreground-only mode on/off 
    sigaction(SIGINT, &ignore_action, NULL);    // ignore so shell not terminated
    sigset_t fgSigs;
    sigemptyset(&fgSigs);
    sigaddset(&fgSigs, SIGTSTP);                // for masking SIGTSTP during fg execution 

    // loop until user enters "exit"
    while (1) { 
        // reap zombies & print completion messages
        ReapZombies(bgPidList, &numBGProcs);
        
        // display prompt & read user input from stdin as single string                        
        ReadInput(userInput, INPMAX);
        if (IsLineToIgnore(userInput))   // ignore blank lines, comments; reprompt immediately
            continue; 
            
        // parse input, store args in array & store count (incl <, >, &)
        Expand$$(userInput);    // expand $$ to PID 
        char* args[ARGMAX] = {NULL};                
        int numArgs = ParseInput(userInput, args);
        
        // execute built-in command (no run in background option)
        char* command = args[0];
        if (strcmp(command, "exit") == 0) {            // exit
            ExitCleanup(bgPidList);
        }
        else if (strcmp(command, "cd") == 0) {        // change directory
            ChangeDirectory(args[1]);     // filepath is only arg
        }
        else if (strcmp(command, "status") == 0) {    // get exit status
            DisplayStatus(childExitMethod);    
        }
        
        // execute non-built-in command
        else {
            int inPathIdx = -1, outPathIdx = -1;    // indices of redir in/out filepaths in args array
            int fdIn, fdOut;                        // file descriptors for stdin/stdout redir
            
            // check if run in background requested; if so, remove "&" from args array
            bool runInBG = IsBGCommand(args, numArgs);
            if (runInBG)
                RemoveArgs(args, (numArgs - 1), 1, &numArgs);   // numArgs-1 is idx of "&"
            
            // fork process; child redirects as approp & exec's command
            pid_t spawnPid = fork();
            switch (spawnPid) {
                case -1: {   // error
                    printf("failed to create child process\n");
                    fflush(stdout);
                    exit(1);
                } break;
                
                case 0: {    // child
                    // inherits ignore SIGINT from parent
                    // ignore SIGTSTP -- only parent handles fg-only mode toggling
                    sigaction(SIGTSTP, &ignore_action, NULL);
                    
                    // stdin redir
                    inPathIdx = FindRedirFilepath(args, "<");
                    if (inPathIdx > 0 && inPathIdx < numArgs) {  // ensure valid index
                        fdIn = open(args[inPathIdx], O_RDONLY);
                        if (fdIn < 0) {
                            printf("cannot open %s for input\n", args[inPathIdx]);
                            fflush(stdout);
                            exit(1);
                        }
                        dup2(fdIn, 0);
                        // remove redir symbol & pathname from args array pre-exec
                        RemoveArgs(args, inPathIdx - 1, 2, &numArgs); 
                    } 
                    // stdout redir
                    outPathIdx = FindRedirFilepath(args, ">");
                    if (outPathIdx > 0 && outPathIdx < numArgs) {  // ensure valid index
                        fdOut = open(args[outPathIdx], O_WRONLY | O_CREAT | O_TRUNC, 0600);
                        if (fdOut < 0) {
                            printf("cannot open %s for output\n", args[outPathIdx]);
                            fflush(stdout);
                            exit(1);
                        }
                        dup2(fdOut, 1);
                        // remove redir symbol & pathname from args array pre-exec
                        RemoveArgs(args, outPathIdx - 1, 2, &numArgs); 
                    } 
                    
                    // run in background if requested & allowed
                    if (runInBG && !fgOnlyMode) {
                        // default to /dev/null for in/out if no filepath specified
                        if (inPathIdx < 0)
                            fdIn = open("/dev/null", O_RDONLY);
                            if (fdIn < 0) {
                                printf("cannot open /dev/null for input\n");
                                fflush(stdout);
                                exit(1);
                            }
                            dup2(fdIn, 0);
                        if (outPathIdx < 0)
                            fdOut = open("/dev/null", O_WRONLY);
                            if (fdOut < 0) {
                                printf("cannot open /dev/null for output\n");
                                fflush(stdout);
                                exit(1);
                            }
                            dup2(fdOut, 1);
                    }
                    // otherwise run in foreground
                    else {
                        // reset to default SIGINT behavior so can terminate self
                        sigaction(SIGINT, &default_action, NULL);
                    }
                    
                    ExecuteCommand(args);
                } break;
                
                default: {   // parent               
                    // don't wait while child runs in bg 
                    if (runInBG && !fgOnlyMode) { 
                        // add child PID to list (also increments count)
                        AddToBGProcList(bgPidList, spawnPid, &numBGProcs);
                        printf("background pid is %d\n", spawnPid);
                        fflush(stdout);
                    }
                    // wait while child runs in fg
                    else {
                        // delay any SIGTSTP until fg process finishes running
                        sigprocmask(SIG_BLOCK, &fgSigs, NULL);
                        waitpid(spawnPid, &childExitMethod, 0);
                        // unblock to allow printing of msg, if approp
                        sigprocmask(SIG_UNBLOCK, &fgSigs, NULL);
                        // check if child was killed by signal
                        if (WIFSIGNALED(childExitMethod)) 
                            DisplayStatus(childExitMethod);
                    }
                } break;
            }
        }
    }
}

// Catches SIGTSTP, toggles foreground only mode on/off, & prints appropriate message.
//    For use by shell. Children should always ignore.
void CatchSIGTSTP(int signo) {
    fgOnlyMode = !fgOnlyMode;   // toggle mode 
    if (fgOnlyMode) {
        char* enterMsg = "\nEntering foreground-only mode (& is now ignored)\n";
        write(STDOUT_FILENO, enterMsg, 50);
    }
    else {
        char* exitMsg = "\nExiting foreground-only mode\n";
        write(STDOUT_FILENO, exitMsg, 30);
    }
}


// Displays prompts and gets user input from stdin. Strips newline and leading spaces and 
//     copies string to passed buffer. 
//     If error due to signal interruption, clears error and attempts to read again.
//     Frees own allocated memory.
//     adapted from: https://oregonstate.instructure.com/courses/1738958/pages/3-dot-3-advanced-user-input-with-getline
void ReadInput(char* userInput, int inpSz) {
    int numCharsEntered = -5;
    size_t bufferSz = 0;        // init to 0 so getline will auto-allocate
    char* lineEntered = NULL;   // init to NULL so getline will auto-allocate

    // clear buffer
    memset(userInput, '\0', inpSz);
    
    while(1) {
        // display command prompt
        printf(": ");
        fflush(stdout);
        
        // get input & reprompt if interrupted
        numCharsEntered = getline(&lineEntered, &bufferSz, stdin);
        if (numCharsEntered == -1)
            clearerr(stdin);
        else
            break;    // input received
    }
    
    // strip newline
    lineEntered[strcspn(lineEntered, "\n")] = '\0';

    // copy read line, excluding leading spaces
    int numSpaces = strspn(lineEntered, " ");
    strncpy(userInput, lineEntered + numSpaces, inpSz);

    free(lineEntered);
    lineEntered = NULL;
}


// Returns true if user input line can be ignored (empty or starts with '#')
bool IsLineToIgnore(char* userInput) {
    bool toIgnore = false;
    if (userInput[0] == '\0' || userInput[0] == '#')  // line of all spaces also reduces to "\0" in ReadInput()
        toIgnore = true;
    return toIgnore;
}


// Parses input string into array of tokens & returns number of tokens
int ParseInput(char* userInput, char* args[]) {
    int t = 0;          // token counter 

    // get first
    char* token = strtok(userInput, " ");
    if (token) {         // don't store NULL
        args[t] = token;
        t++;
    }
    // get rest
    while (token != NULL) {
        if ((token = strtok(NULL, " "))) {     // don't store NULL
            args[t] = token;
            t++;
        }
    }
    return t;  
}


// Expands all instances of "$$" to current PID in passed string.
//   Assumes PID < 10 digits.
//   Uses SubstrReplace() for substring replacement.
void Expand$$(char* userInput) {  
    char remSubstr[] = "$$";
    
    // get PID as string (assumes <10 digits)
    char replSubstr[10];
    memset(replSubstr, '\0', sizeof(replSubstr));
    sprintf(replSubstr, "%d", getpid());
    
    // replace "$$" with PID in string
    SubstrReplace(userInput, remSubstr, replSubstr, INPMAX);
}


// Modifies a string by replacing all instances of a substring with another specified substring.
//   If substring to remove not in string, returns w/o any replacements.
void SubstrReplace(char* origStr, char* remSubstr, char* replSubstr, int strMax) {  
    char* remLoc = NULL;
    // if still instance of substring to remove, get its location
    while ((remLoc = strstr(origStr, remSubstr)))   {      
        char temp[strMax];           // temp string for appending to
        memset(temp, '\0', strMax);
        
        // copy chars up to substring to be removed
        strncpy(temp, origStr, (remLoc - origStr));
        // append replacement substring
        strcat(temp, replSubstr);
        // append any remaining chars after substring
        strcat(temp, remLoc + strlen(remSubstr));
        
        // clear orig string & copy new string
        memset(origStr, '\0', strMax);
        strcpy(origStr, temp);
    }
}


// Terminates & waits for background child processes on list, then exits.
void ExitCleanup(pid_t list[]) { 
    int p = 0;
    int childExitMethod = -5;
    
    // no fg processes running if command prompt was avail to request "exit"
    
    // request termination of & wait for all bg processes                                   
    while (list[p] != -1) {
        kill(list[p], SIGTERM);
        waitpid(list[p], &childExitMethod, 0);
        list[p] = -1;
        p++;
    }      
    fflush(stdout);
	exit(0);
}


// Changes current working dir to that specified by path, or defaults to HOME if path NULL
void ChangeDirectory(char* path) {
    // no path specified, default to HOME
    if (!path)
        chdir(getenv("HOME"));
    // use path specified
    else {
        int result = chdir(path); 
        if (result < 0) {
            printf("%s: no such file or directory\n", path);
            fflush(stdout);
        }
    }
}

// Displays exit status or terminating signal of most recent foreground process. 
//    Returns exit status of 0 if no processes yet run.
void DisplayStatus(int childExitMethod) {
    // no fg process run yet
    if (childExitMethod == -5) {
        printf("exit value 0\n");
        fflush(stdout);
    } 
    // exited
    else if (WIFEXITED(childExitMethod)) {        
        printf("exit value %d\n", WEXITSTATUS(childExitMethod));
        fflush(stdout);
    } 
    // terminated by signal
    else if (WIFSIGNALED(childExitMethod)) {
        printf("terminated by signal %d\n", WTERMSIG(childExitMethod));
        fflush(stdout);
    }
}


// Removes specified number args from array of char*'s starting at given index
//    (by shifting subsequent vals down) & decreases arg count appropriately.
//    Assumes at least one NULL ptr terminates arg array & no NULL vals btwn valid args.
void RemoveArgs(char* args[], int startIdx, int numToRem, int* numArgs) {
    int n;
    for (n = 0; n < numToRem; n++) {
        if (args[startIdx])         // decrement count only if ptr to arg not NULL
            (*numArgs)--;    
        int a;
        for (a = startIdx; args[a] != NULL; a++) {
            args[a] = args[a+1];    // shift down
        }
    }
}


// Searches arguments array & returns true if last arg is "&" (indicating command should 
//    be run in the background); otherwise returns false.
bool IsBGCommand(char* args[], int numArgs) {
    if (strcmp(args[numArgs - 1], "&") == 0) {
        return true;
    }
    return false;
}

// Searches arguments array for specified redirection symbol ("<" or ">" & if found, returns
//    index of next string (associated filepath); otherwise returns -1;
//    Renturns index instead of pointer to string itself so index can be used to remove args 
//    with RemoveArgs().
int FindRedirFilepath(char* args[], char* redirSym) {
    int filepathIdx = -1;
    int a = 0;
    while (args[a]) {
        // if symbol found, return next string (filepath)
        if (strcmp(args[a], redirSym) == 0 && args[a+1] != NULL) {   // ensure something follows symbol
            filepathIdx = a + 1;
            break;
        }
        a++;
    }
    return filepathIdx;
}


// Executes command using args in passed arg vector.
//    Looks for command in PATH variable.
//    If exec fails, prints message and terminates self.
void ExecuteCommand(char** args) {
    if (execvp(*args, args) < 0) {    // *args is first arg (command), args is entire array
        printf("%s: no such file or directory\n", *args);
        fflush(stdout);
        exit(1);
    }
}


// Adds PID to background processes list & increments count.
//    Assumes at least one -1 val terminates PID array. 
void AddToBGProcList(pid_t list[], pid_t pid, int* num) {
    if (*num < PROCMAX - 2) {   // ensure doesn't exceed list cap + room for terminating -1 val
        list[*num] = pid;
        (*num)++;
    }
    else {
        printf("process list full; process not added\n");
        fflush(stdout);
    }
}


// Locates and removes PID from list (by shifting subsequent vals down) & decrements count.
//    Assumes at least one -1 val terminates PID array & no -1 vals between valid PID's.
void RemFromBGProcList(pid_t list[], pid_t pid, int* num) {
    int p;
    for (p = 0; p < (*num); p++) {
        // if pid found
        if (list[p] == pid) {
            // shift down values to overwrite & decrement count
            while (list[p] != -1) {
                list[p] = list[p+1];
                p++;
            }
            (*num)--;
        }
    }
}


// Reaps zombies by looping through list of background processes and waitpid()ing on
//    each. If process has terminated, checks if exited or was terminated by signal, prints
//    appropriate message, and removes from list.
void ReapZombies(pid_t list[], int* num) {    
    int childExitMethod = -5;   // init to unlikely val
    int p;
    
    for (p = 0; p < (*num); p++) {
        // check if proc has terminated
        if (waitpid(list[p], &childExitMethod, WNOHANG)) {
            // exited
            if (WIFEXITED(childExitMethod)) {        
                printf("background pid %d is done: exit value %d\n", 
                        list[p], WEXITSTATUS(childExitMethod));
                fflush(stdout);
            } 
            // terminated by signal
            else if (WIFSIGNALED(childExitMethod)) {
                printf("background pid %d is done: terminated by signal %d\n", 
                        list[p], WTERMSIG(childExitMethod));
                fflush(stdout);
            }
            // remove from list (also decrements count)
            RemFromBGProcList(list, list[p], num); 
        }
    }
} 



