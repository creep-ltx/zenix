#include "libz.h"
int main(void)
{
    char b[128];
    if (sys_getcwd(b, sizeof b) < 0) return 1;
    printf("%s\n", b);
    return 0;
}
