/*****************************************************************************
 * smallsh.c
 * Author: Kyle Martinez (martink9@oregonstate.edu)
 * Description: This file contains a program which implements a very small
 * 				terminal shell. It has 3 built in commands:
 * 					cd - change directory
 * 					status - print the termination status of the last
 *							 foreground process
 *					exit - exits the terminal
 *				Besides these built in commands, the terminal will execute any
 *				other commands provided to it.
 ****************************************************************************/
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

/*****************************************************************************
 * Typedefs/structs
 ****************************************************************************/
typedef enum { false = 0, true = !false } bool;

/*****************************************************************************
 * Constants
 ****************************************************************************/
const int MAX_LINE_LENGTH = 2048;
const int MAX_LINE_ARGS = 512;
const int CMD_NAME = 0;
const int STDIN_NUM = 0;
const int STDOUT_NUM = 1;

/*****************************************************************************
 * Prototypes
 ****************************************************************************/
void printAndFlush(char *line);
char* termPrompt();
char* getUserCmd();
void changeDirectory(char *filepath);
void parseUserCmd(char *userline, char **args, int *argCount, char **input, char **output, bool *backgroundFlag);
void terminatePidGroup(int bgProcs[], int numBgProcs);
void reportExitStatus(int exitMethod);
int openInputFD(char *filepath);
void execute(char **args);
void redirectStdin(int FDNum);
void redirectStdout(int FDNum);
int openInpFile(char *inpfile);
int openOutFile(char *outfile);
void redirectStdIO(char *newStdin, char *newStdout, bool bgFlag);
void catchSIGTSTP(int signo);

// Global variable for signal handling
volatile static bool foregroundOnly = 0;
volatile static int fgPidForSignal = -5;

/*****************************************************************************
 * Main
 ****************************************************************************/
int main(int argc, char**argv)
{
	/*************************
	 * Signal Handlers
	 ************************/
	// set up SIGINT handlers 
	struct sigaction default_action = {{ 0 }}, 
					 SIGTSTP_action = {{ 0 }}, 
					 ignore_action = {{ 0 }};

	default_action.sa_handler = SIG_DFL;
	ignore_action.sa_handler = SIG_IGN;

	// handle SIGTSTP - disabled for children
	SIGTSTP_action.sa_handler = catchSIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = 0;

	// actually ignore SIGINT
	sigaction(SIGTSTP, &SIGTSTP_action, NULL);
	sigaction(SIGINT, &ignore_action, NULL);

	/*************************
	 * Control variables
	 ************************/
	// variables for parsing user input
	char *inputfile = NULL, // redirect stdin to this file
		 *outputfile = NULL, // redirect stdout to this file
		 *userCmd = NULL; // string to capture entire user command line

	int cmdArgCount = 0,	
		childExitMethod = -5,
		savedStdin = dup(STDIN_NUM), // save original stdin
		savedStdout = dup(STDOUT_NUM), // save original stdout
		backgroundPids[100], // array to store background pids in
		backgroundPidCount = 0,
		bgChildExitMethod = -5,
		actualBgPid = 0;

	char *cmdargs[MAX_LINE_ARGS]; // hold an array of args, first one is also the command
	int idx = 0;
	for (idx = 0; idx < MAX_LINE_ARGS; idx++)
	{
		cmdargs[idx] = malloc(MAX_LINE_LENGTH);
		memset(cmdargs[idx], '\0', MAX_LINE_LENGTH);
	}
		
	pid_t forkPid = -5;
	bool background = 0; // flag for background processes - 1=foreground, 0=background

	/*************************
	 * Terminal prompt loop
	 ************************/
	while (1)
	{
		/***************************
		 * Handle background zombies
		 **************************/
		// check background pid tracker for reaping
		for (idx = 0; idx < backgroundPidCount; idx++)
		{
			actualBgPid = waitpid(backgroundPids[idx], &bgChildExitMethod, WNOHANG);

			// if background pid has been reaped, report it.
			if (actualBgPid)
			{
				printf("%d has been reaped.\n", actualBgPid);
				fflush(stdout);

				reportExitStatus(bgChildExitMethod);

				// slide background pids forward if one has been removed
				int j = 0;
				for (j = idx; j < backgroundPidCount; j++)
				{
					backgroundPids[j] = backgroundPids[j + 1];
				}
				// update count
				backgroundPidCount--;
			}
		}

		/***************************
		 * User input
		 **************************/
		// get command from user and parse it for execution
		userCmd = termPrompt();
		parseUserCmd(userCmd, cmdargs, &cmdArgCount, &inputfile, &outputfile, &background);

		/***************************
		 * Result decision path
		 **************************/
		// blank line - must be checked first to avoid the other checks segfaulting
		if (cmdargs[CMD_NAME] == NULL)
		{
			// do nothing, let variables parsing variables reset at end of loop
		}
		// comment line
		else if (cmdargs[CMD_NAME][0] == '#')
		{
			// do nothing, let variables parsing variables reset at end of loop
		}
		// change director command
		else if (!strcmp(cmdargs[CMD_NAME], "cd"))
		{
			changeDirectory(cmdargs[1]);
		}
		// exit command
		else if (!strcmp(cmdargs[CMD_NAME], "exit"))
		{
			terminatePidGroup(backgroundPids, backgroundPidCount);
		}
		// status command
		else if (!strcmp(cmdargs[CMD_NAME], "status"))
		{
			reportExitStatus(childExitMethod);	
		}
		// try to exec the command
		else
		{
			// fork new process and test for success
			forkPid = fork();
			// this block is in the parent
			switch (forkPid)
			{
				// check for failure
				case -1: { perror("Fork failure, get a spoon.\n"); exit(-2); break; }
				// child code block
				case 0:
				{
					// redirect stdin/stdout before exec
					redirectStdIO(inputfile, outputfile, background);

					// restore SIGINT for foreground processes before exec
					if (!background)
					{
						sigaction(SIGINT, &default_action, NULL);
					}	

					// ignore SIGTSTP in all child processes
					sigaction(SIGTSTP, &ignore_action, NULL);

					execute(cmdargs);
					exit(0);
					break;
				}
				default: 
				{
					/* set global equal to forkpid so signal handler waits
					 * for foreground process */

					if (!background || foregroundOnly)
					{
						fgPidForSignal = forkPid;
						forkPid = waitpid(forkPid, &childExitMethod, 0);
						fgPidForSignal = -5;
						if (WIFSIGNALED(childExitMethod))
						{
							reportExitStatus(childExitMethod);
						}
					}
					// add background pid to array for tracking
					else
					{
						backgroundPids[backgroundPidCount] = forkPid;
						backgroundPidCount++;
						printf("PID of new background process: %d\n", forkPid);
						fflush(stdout);
					}

					// reset stdin/stdout to terminal
					redirectStdin(savedStdin);
					redirectStdout(savedStdout);
				}
			}
		}

		/* reset parsing variables for next command. check that they exist
		 * first in the case of a comment line or blank line. */
		if (userCmd)
		{
			free(userCmd);
			userCmd = NULL;
		}
		
		if (inputfile)
		{
			free(inputfile);
			inputfile = NULL;
		}
		
		if (outputfile)
		{
			free(outputfile);
			outputfile = NULL;
		}
				
		/* since we set one of the array elements to NULL, we need to re-allocate
		 * that element */
		cmdargs[cmdArgCount] = malloc(MAX_LINE_LENGTH);
		cmdArgCount = 0;
		background = 0;
	} // end while loop

	// deallocate memory
	idx = 0;
	for (idx = 0; idx < MAX_LINE_ARGS; idx++)
	{
		free(cmdargs[idx]);
	}

	return 0;
}

/*****************************************************************************
 * Description: Displays CLI prompt and waits for user input.
 * Parameters: None
 * Returns: A string of the command the user input
 ****************************************************************************/
char* termPrompt()
{
	printAndFlush(":");
	return getUserCmd();
}

/*****************************************************************************
 * Description: Waits for user to provide input to stdin. Source: Class reading
 * Parameters: None
 * Returns: A string of user input
 ****************************************************************************/
char* getUserCmd()
{
	char *lineEntered = NULL;
	size_t bufferSize = 0;
	int numCharsEntered = 0;

	while (true)
	{
		// get line from user
		numCharsEntered = getline(&lineEntered, &bufferSize, stdin);
		if (numCharsEntered == -1)
			clearerr(stdin);
		else
			break;  // loop control - exits when we have input
	}
	// get rid of newline at end
	lineEntered[numCharsEntered - 1] = '\0';
	return lineEntered;
}

/*****************************************************************************
 * Description: Prints a line and flushes afterward
 * Parameters: line = the line to print
 * Returns: None
 ****************************************************************************/
void printAndFlush(char *line)
{
	printf("%s", line);
	fflush(stdout);
}

void parseUserCmd(char *userline, char **args, int *argCount, char **input, char **output, bool *backgroundFlag)
{
	char *saveptr;
	char* piece = strtok_r(userline, " ", &saveptr);
	int idx = 0;
	int parentPid = getpid();
	/* capture the command the user typed in and advance the token
	 * do this out of the while loop b/c a command should always be a single string
	 * and is not optional */
	if (piece != NULL)
	{
		strcpy(args[0], piece);
		piece = strtok_r(NULL, " ", &saveptr);
		idx++;
	}

	// the while loop captures all the optional items
	while (piece)
	{
		if (!strcmp(piece, "<")) // there is an input redirect
		{
			piece = strtok_r(NULL, " ", &saveptr);
			char *tempStr = malloc(512);
			strcpy(tempStr, piece);
			*input = tempStr;
		}
		else if (!strcmp(piece, ">")) // there is an output redirect
		{
			piece = strtok_r(NULL, " ", &saveptr);
			char *tempStr = malloc(512);
			strcpy(tempStr, piece);
			*output = tempStr;
		}
		else if (!strcmp(piece, "&")) // this piece is our background flag
		{
			*backgroundFlag = true;
		}
		else // this piece is an argument
		{
			char *expanded = NULL;

			int i = 0;
			for (i = 0; i < strlen(piece); i++)
			{
				if (piece[i] == '$')
				{
					if ( (i + 1) < strlen(piece) && piece[i + 1] == '$')
					{
						// expand pid
						piece[i] = '\0';
						expanded = malloc(MAX_LINE_LENGTH);
						sprintf(expanded, "%s%d", piece, parentPid);			
						break;
					}
				}
			}
			if (expanded)
			{
				strcpy(args[idx], expanded);
			}
			else
			{
				strcpy(args[idx], piece);
			}
			idx++;
		}
	
		// advance the token
		piece = strtok_r(NULL, " ", &saveptr);
	}
	*argCount = idx;
	/* free and set the next available slot to NULL so we can pass the 
	 * arg array to execvp */
	free(args[idx]);
	args[idx] = NULL;
}

/*****************************************************************************
 * Description: Changes the CWD to the directory specified by filepath.
 * Parameters: filepath = the location of the directory to switch to
 * Returns: None
 ****************************************************************************/
void changeDirectory(char *filepath)
{
	int failure = 0;
	// check for empty argument
	if (!filepath)
	{
		failure = chdir(getenv("HOME"));
	}			
	// relative filepath
	else
	{
		failure = chdir(filepath);
	}

	if (failure)
	{
		printf("Error with chdir: %d\n", failure);
		fflush(stdout);
	}

}

/*****************************************************************************
 * Description: Terminates the parent process and all child processes
 * Parameters: None
 * Returns: None
 ****************************************************************************/
void terminatePidGroup(int bgProcs[], int numBgProcs)
{
	int exitMethod = -5;

	int i = 0;
	for (i = 0; i < numBgProcs; i++)
	{
		kill(bgProcs[i], SIGTERM);
		waitpid(bgProcs[i], &exitMethod, 0);
	}

	exit(0);
}

/*****************************************************************************
 * Description: Prints exit status to CLI. If exited normally, prints the 
 * 				exit status code. If signaled, prints the signal code
 * Parameters: the int status code to interpret
 * Returns: None
 ****************************************************************************/
void reportExitStatus(int exitMethod)
{
	if (WIFEXITED(exitMethod))
	{
		int exitStatus = WEXITSTATUS(exitMethod);
		printf("Exit Status: %d\n", exitStatus);
		fflush(stdout);
	}
	else if (WIFSIGNALED(exitMethod))
	{
		int termSignal = WTERMSIG(exitMethod);
		if (termSignal != 123)
		{
			printf("Terminating Signal: %d\n", termSignal);
			fflush(stdout);
		}
	}
}

/*****************************************************************************
 * Description: Executes a command, searching PATH for command. 
 * 				Source: Class Lecture
 * Parameters: An array of arguments/the command to execute
 * Returns: None
 ****************************************************************************/
void execute(char **args)
{
	if (execvp(args[CMD_NAME], args))
	{
		perror("Exec Failure!!!\n");
		exit(1);
	}
}

/*****************************************************************************
 * Description: Opens a file to have stdin redirected to it. Opens as read-only
 * 				Source: Class Lecture
 * Parameters: inpfile = the filename to be opened
 * Returns: int representing the open file descriptor
 ****************************************************************************/
int openInpFile(char *inpfile)
{
	// create file descriptor to redirect stdin
	int inputFD = open(inpfile, O_RDONLY);
	if (inputFD == -1) { perror("Input file could not be opened"); exit(1); }

	return inputFD;
}

/*****************************************************************************
 * Description: Redirects stdin to a provided file descriptor number. Exits
 * 				if redirection fails.
 * 				Source: Class Lecture
 * Parameters: FDNum = the file descriptor to be redirected to
 * Returns: None
 ****************************************************************************/
void redirectStdin(int FDNum)
{
	int inResult = 0;
	// redirect stdin
	inResult = dup2(FDNum, STDIN_NUM);
	if (inResult == -1) { perror("Input redirection failed.\n"); exit(1); }
}

/*****************************************************************************
 * Description: Opens a file to have stdout redirected to it. Opens for writing,
 * 				for truncating, and creating. Exits if file open fails
 * 				Source: Class Lecture
 * Parameters: outfile = the filename to be opened
 * Returns: int representing the open file descriptor
 ****************************************************************************/
int openOutFile(char *outfile)
{
	// create file descriptor to redirect stdin
	int outputFD = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (outputFD == -1) { perror("Output file could not be opened"); exit(1); }

	return outputFD;
}

/*****************************************************************************
 * Description: Redirects stdout to a provided file descriptor number. Exits
 * 				if redirection fails.
 * 				Source: Class Lecture
 * Parameters: FDNum = the file descriptor to be redirected to
 * Returns: None
 ****************************************************************************/
void redirectStdout(int FDNum)
{
	// redirect stdout
	int outResult = 0;
		
	// redirect stdout
	outResult = dup2(FDNum, STDOUT_NUM);
	if (outResult == -1) { perror("Output redirection failed.\n"); exit(1); }
}

/*****************************************************************************
 * Description: Toggle global variable
 *
 * Parameters: signo = the parameter for passing which signal caused the 
 * 					   handler to start
 * Returns: None
 ****************************************************************************/
void catchSIGTSTP(int signo)
{
	int exitMeth = -5;
	//if (fgPidForSignal != -5)
	{
		waitpid(fgPidForSignal, &exitMeth, 0);
	}
	if (!foregroundOnly)
	{
		foregroundOnly = true;
		char *msg = "\nEntering foreground only mode. (& is now ignored)\n:";
		if (fgPidForSignal != -5)
		{
			write(STDOUT_FILENO, msg, 51);
		}
		else
		{
			write(STDOUT_FILENO, msg, 52);
		}
						
	}
	else
	{
		foregroundOnly = false;
		char *msg = "\nExiting foreground only mode.\n:";
		if (fgPidForSignal != -5)
		{
			write(STDOUT_FILENO, msg, 31);
		}
		else
		{
			write(STDOUT_FILENO, msg, 32);
		}
	}

}

/*****************************************************************************
 * Description: Handles redirection of stdin and stdout based on the status of
 * 				'newStdin', 'newStdout', and bgFlag. Redirects stdin and stdout
 * 				to 'newStdin' and 'newStdout', respectively. If the process is
 * 				a background process, redirects to dev/null if 'newStdin' and
 * 				'newStdout' are NULL. 
 * Parameters: newStdin = the filename of the file to redirect stdin to
 * 			   newStdout = the filename of the file to redirect stdout to
 * 			   bgFlag = a boolean flag - 1 = backgroung process 0 = foreground
 * Returns: None
 ****************************************************************************/
void redirectStdIO(char *newStdin, char *newStdout, bool bgFlag)
{
	// redirect stdin
	if (newStdin != NULL)
	{
		// create file descriptor to redirect stdin
		int inputFD = openInpFile(newStdin);
		// redirect stdin
		redirectStdin(inputFD);
	}
	else if (bgFlag)
	{
		// redirect to dev null
		int devNull = open("/dev/null", O_RDONLY);
		redirectStdin(devNull);
	}

	// redirect stdout
	if (newStdout != NULL)
	{
		// create file descriptor to redirect stdin
		int outputFD = openOutFile(newStdout);
		// redirect stdout
		redirectStdout(outputFD);
	}
	else if (bgFlag)
	{
		// redirect to dev null
		int devNull = open("/dev/null", O_WRONLY);
		redirectStdout(devNull);
	}
}
