#include <kernel/types.h>
#include <user/user.h>

int main(int, const char*[])
{
    uint64 free_bytes = freemem();

    fprintf(2, "%ld bytes\n", free_bytes);

    return 0;
}