#include <kmon/panic.h>
#include <panic.h>

void kmon_panic(void){
    panic("Manual panic requested from kernel monitor.", 0);
}
