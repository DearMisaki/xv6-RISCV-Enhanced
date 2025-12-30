#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

#define DATASIZE 32 * 4096

int
main(int argc, char *argv[])
{
  // Your code here.
  
  char *data = sbrk(DATASIZE);

  const char* tag = "This may help.";

  for (int i = 0; i < DATASIZE - 14 + 1; ++i)
  {
    if (data[i] != 'T')
    {
      continue;
    }

    int found = 1;

    for (int j = 0; j < 14; ++j)
    {
      if (data[i + j] != tag[j])
      {
        found = 0;
        break;
      }
    }
    
    if (!found)
    {
      continue;
    }

    char* secret = &data[i + 16];

    int secret_len = 0;

    while (1)
    {
      char c = secret[secret_len];
      
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      {
        secret_len++;
      }
      else
      {
        break;
      }
    }
    
    if (secret_len > 0) 
    {
      write(1, secret, secret_len);
      write(1, "\n", 1);
      exit(0);
    }
    
  }

  exit(1);
}
