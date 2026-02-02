
/******************************************************************
 * Author : Pranav Shastry
 * Email  : pranav.shastry@colorado.edu
 * Date   : Feb 01st 2026
 *
 * Notes  :
 * Assignment 3 part 1 code had been developed on the existing
 * starter code which was available in the repo.
 *
 * AI Declaration :
 * ChatGPT LLM was used during the development of the 3 functions.
 * Exchange of back and forth prompts occured in order to make
 * the functions comply with what the assignment exactly specifies.
 * I reviewed each line of the code, made corrections wherever
 * necessary.
 *
 * Link To Chat History :
 * https://chatgpt.com/share/697fef67-8e78-8010-a16a-e091449b9870
 *
 *****************************************************************/




#include "systemcalls.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>



/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{
    if(cmd == NULL)
    {
        return false;
    }

    errno = 0;
    int status = system(cmd);

    // system() failed to create a child, failed to fork, etc.
    if(status == -1)
    {
        return false;
    }

    // If the child terminated normally, check its exit code.
    if(WIFEXITED(status))
    {
        return (WEXITSTATUS(status) == 0);
    }

    // If it didn't exit normally (e.g., killed by a signal, stopped),
    // treat that as failure per your spec.
    return false;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);

    char *command[count + 1];
    int i;

    for(i = 0; i < count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

    // Optional: basic input validation
    if(count < 1 || command[0] == NULL)
    {
        va_end(args);
        return false;
    }

    /*
     * Execute a system command by calling fork, execv(),
     * and waitpid instead of system.
     * Use command[0] as the full path to the command to execute,
     * and the full command[] vector as argv (NULL terminated).
     */

    pid_t pid = fork();
    if(pid < 0)
    {
        // fork failed
        va_end(args);
        return false;
    }

    if(pid == 0)
    {
        // Child: execv replaces the process image on success
        execv(command[0], command);

        // If execv returns, it failed. Use _exit (not exit).
        _exit(127);
    }

    // Parent: wait for the child to complete
    int status = 0;
    pid_t w;
    do
    {
        w = waitpid(pid, &status, 0);
    }
    while(w == -1 && errno == EINTR);

    if(w == -1)
    {
        // waitpid failed
        va_end(args);
        return false;
    }

    va_end(args);

    // Return true only if child exited normally with status 0
    if(WIFEXITED(status))
    {
        return (WEXITSTATUS(status) == 0);
    }

    // If terminated by signal or otherwise not a normal exit, fail
    return false;
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);

    char *command[count + 1];
    int i;
    for(i = 0; i < count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

    // Basic validation (recommended)
    if(outputfile == NULL || count < 1 || command[0] == NULL)
    {
        va_end(args);
        return false;
    }

    pid_t pid = fork();
    if(pid < 0)
    {
        // fork failed
        va_end(args);
        return false;
    }

    if(pid == 0)
    {
        // ---- Child ----
        // Open (or create) output file for stdout redirection.
        // Common behavior: truncate existing file.
        int fd = open(outputfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if(fd < 0)
	{
            _exit(127);
        }

        // Redirect STDOUT to the file.
        if(dup2(fd, STDOUT_FILENO) < 0)
	{
            close(fd);
            _exit(127);
        }

        // fd is no longer needed after dup2 (stdout now points to the file)
        close(fd);

        // Execute the command. command[0] must be an absolute path per your spec.
        execv(command[0], command);

        // If execv returns, it failed.
        _exit(127);
    }

    // ---- Parent ----
    int status = 0;
    pid_t w;
    do
    {
        w = waitpid(pid, &status, 0);
    }
    while (w == -1 && errno == EINTR);

    va_end(args);

    if(w == -1)
    {
        // waitpid failed
        return false;
    }

    // Success only if child exited normally with exit status 0
    if(WIFEXITED(status))
    {
        return (WEXITSTATUS(status) == 0);
    }

    return false;
}
