#include <kmon/panic.h>
#include <panic.h>

void kmon_panic(void){
    panic_exception("Manual panic requested from kernel monitor.", 0);
}
