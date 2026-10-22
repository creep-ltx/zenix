#include "libz.h"
int main(void) { const char *u = getenv("USER"); printf("%s\n", u ? u : "root"); return 0; }
