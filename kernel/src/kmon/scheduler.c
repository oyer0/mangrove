#include <kmon/scheduler.h>
#include <scheduler.h>

void kmon_scheduler(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    scheduler_dump();
}
