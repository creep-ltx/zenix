#include <stdio.h>
#include <math.h>
#include <unistd.h>

/* newlib + hard-float smoke test: printf %f exercises dtoa/malloc,
 * atan runs natively on the 68882 */
int main(int argc, char *argv[])
{
    printf("hello: pid %d, argc %d (argv[0]=%s)\n", getpid(), argc, argv[0]);
    printf("hello: pi ~= %.6f (68882 FPATAN says so)\n", 4.0 * atan(1.0));
    return 42;
}
