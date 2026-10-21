#include "usys.h"

static void run(const char *name)
{
    puts_("init: starting "); puts_(name); puts_("\n");
    int pid = sys_spawn(name);
    if (pid < 0) { puts_("init: spawn failed\n"); return; }
    int code = sys_wait(pid);
    puts_("init: "); puts_(name); puts_(" (pid "); putnum(pid);
    puts_(") exited with "); putnum(code); puts_("\n");
}

int main(void)
{
    puts_("init: pid "); putnum(sys_getpid()); puts_(" running in user mode\n");
    run("hello");
    run("badptr");
    int sp = sys_spawn("spin");          /* runs alongside, preempted */
    run("crash");
    puts_("init: spin survived the crash, waiting for it\n");
    putnum(sys_wait(sp)); puts_(" <- spin exit\n");
    puts_("init: all done\n");
    return 0;
}
