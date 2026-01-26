/*******************************************************
 * Assignment 2 - ECEN 5713
 * Advanced Embedded Software Development
 *
 * Author : Pranav V Shastry
 * Email  : pranav.shastry@colorado.edu
 *
 * Rev    : v1
 * Date   : 25th January 2026
 *
 *
 * Program: writer
 * ----------------
 * This program writes a specified string to a specified
 * file. It is a C-based replacement for the writer.sh
 * script from Assignment 1, using standard C file I/O
 * and syslog for logging.
 *
 *
 * Notes  : AI Declaration
 * The functions I have written were reviewed by OpenAI
 * and no major errors were found. AI has been used for
 * generating the comments and they were reviewed by me
 * (Pranav V Shastry).
 *
*******************************************************/



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>


int main(int argc, char *argv[])
{
    // LOG_USER facility
    openlog("writer", LOG_PID, LOG_USER);

    /*
     * Validate command-line arguments.
     * Expected usage:
     *   writer <writefile> <writestr>
     */
    if(argc != 3)
    {
        syslog(LOG_ERR, "Usage: %s <writefile> <writestr>", argv[0]);
        closelog();
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr  = argv[2];

    /*
     * Open the target file in write mode.
     * The directory is assumed to already exist.
     */
    FILE *fp = fopen(writefile, "w");
    if(fp == NULL)
    {
        syslog(LOG_ERR, "Error opening file '%s': %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    // LOG_DEBUG message
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    /*
     * Write the provided string to the file.
     * Any write failure is logged as an error.
     */
    if(fprintf(fp, "%s", writestr) < 0)
    {
        syslog(LOG_ERR, "Error writing to file '%s': %s", writefile, strerror(errno));
        fclose(fp);
        closelog();
        return 1;
    }

    // Close the file and check for any close-related errors.
    if(fclose(fp) != 0)
    {
        syslog(LOG_ERR, "Error closing file '%s': %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}

