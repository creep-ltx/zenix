#include "libz.h"
int main(void) { const char *u = getenv("USER"); if (!u) u = "root"; printf("uid=0(%s) gid=0(%s)\n", u, u); return 0; }
