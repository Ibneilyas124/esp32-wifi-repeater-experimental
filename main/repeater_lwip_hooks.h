#ifndef REPEATER_LWIP_HOOKS_H
#define REPEATER_LWIP_HOOKS_H

#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/ip4.h"

#ifdef __cplusplus
extern "C" {
#endif

int repeater_can_forward_ipv4(uint32_t src_addr);

#ifdef __cplusplus
}
#endif

#define LWIP_HOOK_IP4_CANFORWARD(p, dest) \
    repeater_can_forward_ipv4(ip4_addr_get_u32(ip4_current_src_addr()))

#endif
