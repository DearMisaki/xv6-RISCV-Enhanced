#include "kernel/types.h"
#include "kernel/fcntl.h"

#include "user/user.h"

void prime(int *lpipe)
{
    int firstNum;
    if (read(lpipe[0], &firstNum, sizeof(firstNum)) <= 0)
    {
        // fprintf(2, "Error: read pipe");
        return;
    }

    fprintf(1, "prime %d\n", firstNum);

    int currentNum;

    int rpipe[2];
    pipe(rpipe);

    if (fork() == 0)
    {
        close(rpipe[1]);
        prime(rpipe);
    }
    else
    {
        close(rpipe[0]);

        while (read(lpipe[0], &currentNum, sizeof(currentNum)))
        {
            // 不能被当前数整除
            if (currentNum % firstNum)
            {
                if (write(rpipe[1], &currentNum, sizeof(currentNum)) < 0)
                {
                    fprintf(2, "Error: write pipe");
                    exit(1);
                }
            }
        }

        close(lpipe[0]);
        close(rpipe[1]);
        wait(0);
    }

    exit(0);
}

void main(int argc, char *argv[])
{
    int p[2];

    pipe(p);

    for (int i = 2; i <= 35; ++i)
    {
        if (write(p[1], &i, sizeof(i)) < 0)
        {
            fprintf(2, "Error: send 35 data.\n");
            exit(1);
        }
    }

    if (fork() == 0)
    {
        close(p[1]);
        prime(p);
    }
    else
    {
        close(p[0]);
        close(p[1]);
        wait(0);
    }
    exit(0);
}