#include <stdio.h>
#include "usys.h"

struct udirent {
    char name[16];
    unsigned int size;
};

int main(void)
{
    struct udirent de;
    unsigned total = 0;
    for (long i = 0; __sys(SYS_readdir, i, (long)&de, 0) == 0; i++) {
        printf("%-14s %8u\n", de.name, de.size);
        total += de.size;
    }
    printf("%18u bytes\n", total);
    return 0;
}
