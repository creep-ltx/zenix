#include "usys.h"
int main(void)
{
    for (int i = 0; i < 3; i++) {
        puts_("hello from pid "); putnum(sys_getpid()); puts_(" round "); putnum(i); puts_("\n");
        sys_sleep(20);
    }
    return 7;
}
