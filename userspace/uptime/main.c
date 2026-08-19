#include <mangrove.h>
#include <stdio.h>

int main(void)
{
    u64 seconds = uptime_ms() / 1000ULL;
    u64 days = seconds / 86400ULL;
    u64 hours;
    u64 minutes;
    bool printed = false;

    seconds %= 86400ULL;
    hours = seconds / 3600ULL;
    seconds %= 3600ULL;
    minutes = seconds / 60ULL;
    seconds %= 60ULL;

    if (days != 0) {
        printf("%llud", days);
        printed = true;
    }
    if (hours != 0) {
        printf("%s%lluh", printed ? " " : "", hours);
        printed = true;
    }
    if (minutes != 0) {
        printf("%s%llum", printed ? " " : "", minutes);
        printed = true;
    }
    if (seconds != 0 || !printed) printf("%s%llus", printed ? " " : "", seconds);
    printf("\n");
    return 0;
}
