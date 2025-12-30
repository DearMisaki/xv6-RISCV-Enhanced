#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/stat.h"

#include "user/user.h"

char buffer[512];

void find(const char* path, const char *fileName)
{
    int fd;
    struct stat st;
    
    if ((fd = open(path, O_RDONLY)) < 0)
    {
        fprintf(2, "ERROR: open.\n");
        exit(1);
    }

    if (fstat(fd, &st) < 0 )
    {
        fprintf(2, "ERROR: fstat.\n");
        exit(1);
    }

    if (st.type != T_DIR)
    {
        fprintf(2, "ERROR: Not a Dir.\n");
        exit(1);
    }

    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buffer)
    {
        fprintf(2, "ERROR: Path is too long.\n");
        exit(1);
    }

    struct dirent de;

    strcpy(buffer, path);

    char* p = buffer + strlen(buffer);

    *p++ = '/';

    while (read(fd, &de, sizeof de) == sizeof de)
    {
        if (de.inum == 0)   
        {
            continue;
        }

        memmove(p, de.name, sizeof de.name);

        p[DIRSIZ] = 0;

        if (stat(buffer, &st) < 0)
        {
            fprintf(2, "ERROR: stat.\n");
        }

        if (st.type == T_DIR && strcmp(p, ".") != 0 && strcmp(p, "..") != 0)
        {
            find(buffer, fileName);
        }
        else if (strcmp(p, fileName) == 0)
        {
            fprintf(1, "%s\n", buffer);
        }
    }
}

void main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(2, "Error: invalid arguments.\n");
        exit(1);
    }

    find(argv[1], argv[2]);

    exit(0);
}