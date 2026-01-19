// File: writer.c
// Author: Jordan Kooyman
// Date Modified: 2026-01-19
// Description: Simple CLI utility which will create a file at a given path and store the given string in that file
// Basic Code outline generated using ChatGPT: https://chatgpt.com/share/696e63d0-4afc-8007-833e-dc309149e77a

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

#define EXIT_ERROR (1)

int main(int argc, char *argv[])
{
	openlog("writer", 0, LOG_USER);
	
    FILE *writefile = NULL;
    char* writestr;

    /* Validate arguments */
    if (argc != 3) {
		// Write error message to stderr stream
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        syslog(LOG_ERR, "Invalid number of arguments: expected 2 but got %d", (argc - 1));
        closelog();
        return EXIT_ERROR;
    }


    /* Open output file */
    writefile = fopen(argv[1], "w");
    if (writefile == NULL) {
        perror("Error opening output file");
        syslog(LOG_ERR, "Error occured when opening file: %s", argv[1]);
        closelog();
        return EXIT_ERROR;
    }


    /* Write to File */
    syslog(LOG_DEBUG, "Writing '%s' to '%s'", argv[2], argv[1]);
    // ChatGPT reference used to correctly understand file writing formating: https://chatgpt.com/s/t_696e661cd15c8191abf1c46dbd15ffe2
    writestr = argv[2];
    fputs(writestr, writefile);
    fputs("\n", writefile);


    /* Cleanup */
    fclose(writefile);
    closelog();

    return EXIT_SUCCESS;
}
