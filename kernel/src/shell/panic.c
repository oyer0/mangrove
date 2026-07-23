#include <shell/panic.h>
#include <panic.h>

void shell_panic(void){
    panic("Manual panic requested from kernel monitor.", 0);
}
