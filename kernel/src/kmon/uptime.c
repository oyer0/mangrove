#include <kmon/uptime.h>

#include <kprint.h>

void shell_uptime(void){
    u64 total_seconds = timer_uptime_ms() / 1000;
    
    u64 days = total_seconds / 86400;
    u64 hours = (total_seconds % 86400) / 3600;
    u64 minutes = (total_seconds % 3600) / 60;
    u64 seconds = total_seconds % 60;

    if (days > 0)
    {
        kprint("Uptime: %ud %uh %um %us\n",
            (u32)days,
            (u32)hours,
            (u32)minutes,
            (u32)seconds);
    }
    else if (hours > 0)
    {
        kprint("Uptime: %uh %um %us\n",
            (u32)hours,
            (u32)minutes,
            (u32)seconds);
    }
    else if (minutes > 0)
    {
        kprint("Uptime: %um %us\n",
            (u32)minutes,
            (u32)seconds);
    }
    else
    {
        kprint("Uptime: %us\n",
            (u32)seconds);
    }
}
