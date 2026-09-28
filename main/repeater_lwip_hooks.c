#include "repeater_lwip_hooks.h"

int repeater_can_forward_ipv4(uint32_t src_addr)
{
    (void)src_addr;
    return 1;
}
