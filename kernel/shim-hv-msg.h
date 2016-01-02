#ifndef __RLITE_SHIM_HV_MSG_H__
#define __RLITE_SHIM_HV_MSG_H__

/*
 * When compiling from userspace include <stdint.h>,
 * when compiling from kernelspace include <linux/types.h>
 */
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include "rlite/common.h"
#include "rlite/utils.h"


enum {
    RLITE_SHIM_HV_FA_REQ = 1,
    RLITE_SHIM_HV_FA_RESP, /* 2 */

    RLITE_SHIM_HV_MSG_MAX,
};

/* Numtables for shim-hv <==> shim-hv messages exchange. */

extern struct rlite_msg_layout rlite_shim_hv_numtables[RLITE_SHIM_HV_MSG_MAX+1];

/* Message to allocate a new flow. */
struct rlite_hmsg_fa_req {
    rl_msg_t msg_type;
    uint32_t event_id;

    rl_port_t src_port;
    struct rina_name src_appl;
    struct rina_name dst_appl;
} __attribute__((packed));

/* Message to respond to a flow allocation request. */
struct rlite_hmsg_fa_resp {
    rl_msg_t msg_type;
    uint32_t event_id;

    rl_port_t src_port;
    rl_port_t dst_port;
    uint8_t response;
} __attribute__((packed));

#endif  /* __RLITE_SHIM_HV_MSG_H__ */
