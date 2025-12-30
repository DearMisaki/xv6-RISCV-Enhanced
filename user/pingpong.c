#include "kernel/types.h"
#include "kernel/fcntl.h"

#include "user/user.h"

const int BUFFER_SIZE = 1;
const int COUNT = 10000;
const int TICKS_PER_SEC = 100;

void main(int argc, char *argv[])
{
    int pipe1[2], pipe2[2];

    pipe(pipe1);
    pipe(pipe2);

    char byte = 'a';

    int pid = fork();

    if (pid < 0)
    {
        fprintf(2, "Error");
        exit(1);
    }
    else if (pid == 0)
    {
        close(pipe1[1]);
        close(pipe2[0]);

        for (int i = 0; i < COUNT; ++i)
        {
            if (read(pipe1[0], &byte, 1) != 1)
            {
                fprintf(2, "Error.\n");
                exit(1);
            }

            if (write(pipe2[1], &byte, 1) != 1)
            {
                fprintf(2, "Error.\n");
                exit(1);
            }
        }

        close(pipe1[0]);
        close(pipe2[1]);
        exit(0);
    }
    else
    {
        close(pipe1[0]);
        close(pipe2[1]);

        int start = uptime();

        for (int i = 0; i < COUNT; ++i)
        {
            if (write(pipe1[1], &byte, 1) != 1)
            {
                fprintf(2, "Error.\n");
                exit(1);
            }

            if (read(pipe2[0], &byte, 1) != 1)
            {
                fprintf(2, "Error.\n");
                exit(1);
            }
        }

        int end = uptime();

        wait(0);

        close(pipe1[1]);
        close(pipe2[0]);

        int durations = end - start;

        fprintf(1, "%d tick used, performance %d exchange/second.\n", durations, COUNT * TICKS_PER_SEC / durations);

        exit(0);
    }
}