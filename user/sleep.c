#include "kernel/types.h"
#include "kernel/fcntl.h"

#include "user/user.h"


void
main(int argc, char* argv[])
{
    if (argc <= 1)
    {
        fprintf(2, "sleep: sleep for ? seconds.\n");
        exit(1);
    }

    if (argc >= 3)
    {
        fprintf(2, "sleep: only recieve 1 number.\n");
        exit(1);
    }

    int sleepTime = atoi(argv[1]);

    pause(sleepTime);

    exit(0);
}

