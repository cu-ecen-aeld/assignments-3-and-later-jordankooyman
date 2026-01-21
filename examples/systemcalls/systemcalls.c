#include "systemcalls.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdlib.h>

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{
	int retval = 0;
	retval = system(cmd);
	
	if(retval != 0)
	{
		return false;
	}

    return true;
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
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

	// Following code block written with guidance from ChatGPT: https://chatgpt.com/share/69712993-15bc-8007-b18c-9d87b47cb5be
	pid_t pid = fork();
	if(pid < 0) // fork() failed
	{
		perror("Fork");
		return false;
	}
	
	if(pid == 0) // child process task
	{
		execv(command[0], command);
		perror("execv");
		_exit(127); // return code 127 if execv() fails
	}
	
	// Remaining parent process task
	int status;
	if(waitpid(pid, &status, 0) < 0)
	{
		return false; // Waiting failed for some reason
	}
	
	// If execv succeeded, the child would exit with 0
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) 
    { // Command run successfully
		va_end(args);
        return true;
    }

	// Otherwise, command failed
    va_end(args);
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
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

/*
 * TODO
 *   Call execv, but first using https://stackoverflow.com/a/13784315/1446624 as a refernce,
 *   redirect standard out to a file specified by outputfile.
 *   The rest of the behaviour is same as do_exec()
 *
*/
	// Following code block written with reference from: https://stackoverflow.com/a/13784315/1446624
	int fd = open(outputfile, O_WRONLY|O_TRUNC|O_CREAT, 0644);
	if(fd < 0)
	{ // Opening file failed
		perror("File open");
		return false;
	}

	// Following code block written with guidance from ChatGPT: https://chatgpt.com/share/69712993-15bc-8007-b18c-9d87b47cb5be
	pid_t pid = fork();
	if(pid < 0) // fork() failed
	{
		perror("Fork");
		return false;
	}
	
	if(pid == 0) // child process task
	{
		if (dup2(fd, 1) < 0) { perror("dup2"); abort(); } // duplicate fd to replace stdout
		close(fd); // Child does not need this file any more
		execv(command[0], command);
		perror("execv");
		_exit(127); // return code 127 if execv() fails
	}
	
	// Remaining parent process task
	int status;
	if(waitpid(pid, &status, 0) < 0)
	{
		return false; // Waiting failed for some reason
	}
	
	// If execv succeeded, the child would exit with 0
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) 
    { // Command run successfully
		va_end(args);
        return true;
    }

	// Otherwise, command failed
    va_end(args);
    return false;
}
