#include "libz.h"
int main(void) { const char *u = getenv("HOSTNAME"); printf("%s\n", u ? u : "zenix"); return 0; }
