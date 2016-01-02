#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <assert.h>
#include "rlite/kernel-msg.h"
#include "rlite/conf-msg.h"
#include "rlite/utils.h"

#include "rlite/list.h"
#include "rlite/evloop.h"
#include "rlite/appl.h"


static int
flow_allocate_resp_arrived(struct rlite_evloop *loop,
                           const struct rlite_msg_base_resp *b_resp,
                           const struct rlite_msg_base *b_req)
{
    struct rl_kmsg_fa_req *req =
            (struct rl_kmsg_fa_req *)b_req;
    struct rl_kmsg_fa_resp_arrived *resp =
            (struct rl_kmsg_fa_resp_arrived *)b_resp;
    char *local_s = NULL;
    char *remote_s = NULL;

    local_s = rina_name_to_string(&req->local_appl);
    remote_s = rina_name_to_string(&req->remote_appl);

    if (resp->result) {
        PE("Failed to allocate a flow between local appl "
               "'%s' and remote appl '%s'\n",
                local_s, remote_s);
    } else {
        PI("Allocated flow between local appl "
               "'%s' and remote appl '%s' [port-id = %u]\n",
                local_s, remote_s, resp->port_id);
    }

    if (local_s) {
        free(local_s);
    }

    if (remote_s) {
        free(remote_s);
    }

    return 0;
}

static int
flow_allocate_req_arrived(struct rlite_evloop *loop,
                          const struct rlite_msg_base_resp *b_resp,
                          const struct rlite_msg_base *b_req)
{
    struct rlite_appl *appl = container_of(loop,
                                       struct rlite_appl, loop);
    struct rl_kmsg_fa_req_arrived *req =
            (struct rl_kmsg_fa_req_arrived *)b_resp;
    struct rlite_pending_flow_req *pfr = NULL;

    assert(b_req == NULL);
    pfr = malloc(sizeof(*pfr));
    if (!pfr) {
        PE("Out of memory\n");
        /* Negative flow allocation response. */
        return rlite_flow_allocate_resp(appl, req->kevent_id,
                                        req->ipcp_id, 0xffff,
                                        req->port_id, RLITE_ERR);
    }

    pfr->kevent_id = req->kevent_id;
    pfr->ipcp_id = req->ipcp_id;
    pfr->port_id = req->port_id;
    rina_name_copy(&pfr->remote_appl, &req->remote_appl);
    rina_name_copy(&pfr->local_appl, &req->local_appl);
    rina_name_copy(&pfr->dif_name, &req->dif_name);

    pthread_mutex_lock(&appl->lock);
    list_add_tail(&pfr->node, &appl->pending_flow_reqs);
    pthread_cond_signal(&appl->flow_req_arrived_cond);
    pthread_mutex_unlock(&appl->lock);

    PI("port-id %u\n", req->port_id);

    return 0;
}

static int
appl_register_resp(struct rlite_evloop *loop,
                   const struct rlite_msg_base_resp *b_resp,
                   const struct rlite_msg_base *b_req)
{
    struct rl_kmsg_appl_register_resp *resp =
            (struct rl_kmsg_appl_register_resp *)b_resp;
    char *appl_name_s = NULL;

    (void)b_req;

    appl_name_s = rina_name_to_string(&resp->appl_name);

    if (resp->response) {
        PE("Application '%s' %sregistration failed\n", appl_name_s,
            (resp->reg ? "" : "un"));
    } else {
        PD("Application '%s' %sregistration successfully completed\n",
           appl_name_s, (resp->reg ? "" : "un"));
    }

    if (appl_name_s) {
        free(appl_name_s);
    }

    return 0;
}

/* The table containing all kernel response handlers, executed
 * in the event-loop context.
 * Response handlers must not call rlite_issue_request(), in
 * order to avoid deadlocks.
 * These would happen because rlite_issue_request() may block for
 * completion, and is waken up by the event-loop thread itself.
 * Therefore, the event-loop thread would wait for itself, i.e.
 * we would have a deadlock. */
static rlite_resp_handler_t rlite_kernel_handlers[] = {
    [RLITE_KER_FA_REQ_ARRIVED] = flow_allocate_req_arrived,
    [RLITE_KER_FA_RESP_ARRIVED] = flow_allocate_resp_arrived,
    [RLITE_KER_APPL_REGISTER_RESP] = appl_register_resp,
    [RLITE_KER_MSG_MAX] = NULL,
};

struct rl_kmsg_appl_register_resp *
rlite_appl_register_req(struct rlite_appl *appl, uint32_t event_id,
                        unsigned int wait_ms,
                        int reg, unsigned int ipcp_id,
                        const struct rina_name *appl_name)
{
    struct rl_kmsg_appl_register *req;
    int result;

    /* Allocate and create a request message. */
    req = malloc(sizeof(*req));
    if (!req) {
        PE("Out of memory\n");
        return NULL;
    }

    memset(req, 0, sizeof(*req));
    req->msg_type = RLITE_KER_APPL_REGISTER;
    req->event_id = event_id;
    req->ipcp_id = ipcp_id;
    req->reg = reg;
    rina_name_copy(&req->appl_name, appl_name);

    PD("Requesting appl %sregistration...\n", (reg ? "": "un"));

    return (struct rl_kmsg_appl_register_resp *)
           rlite_issue_request(&appl->loop, RLITE_RMB(req),
                               sizeof(*req), 1, wait_ms, &result);
}

void
rlite_flow_spec_default(struct rlite_flow_spec *spec)
{
    memset(spec, 0, sizeof(*spec));
    strncpy(spec->cubename, "unrel", sizeof(spec->cubename));
}

/* This is used by uipcp, not by appl. */
void
rlite_flow_cfg_default(struct rlite_flow_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->partial_delivery = 0;
    cfg->incomplete_delivery = 0;
    cfg->in_order_delivery = 0;
    cfg->max_sdu_gap = (uint64_t)-1;
    cfg->dtcp_present = 0;
    cfg->dtcp.fc.fc_type = RLITE_FC_T_NONE;
}

static struct rl_kmsg_fa_resp_arrived *
flow_allocate_req(struct rlite_appl *appl, uint32_t event_id,
                  unsigned int wait_ms, uint16_t ipcp_id,
                  uint16_t upper_ipcp_id,
                  const struct rina_name *local_appl,
                  const struct rina_name *remote_appl,
                  const struct rlite_flow_spec *flowspec, int *result)
{
    struct rl_kmsg_fa_req *req;

    /* Allocate and create a request message. */
    req = malloc(sizeof(*req));
    if (!req) {
        PE("Out of memory\n");
        *result = -1;
        return NULL;
    }

    memset(req, 0, sizeof(*req));
    req->msg_type = RLITE_KER_FA_REQ;
    req->event_id = event_id;
    req->ipcp_id = ipcp_id;
    req->upper_ipcp_id = upper_ipcp_id;
    if (flowspec) {
        memcpy(&req->flowspec, flowspec, sizeof(*flowspec));
    } else {
        rlite_flow_spec_default(&req->flowspec);
    }
    rina_name_copy(&req->local_appl, local_appl);
    rina_name_copy(&req->remote_appl, remote_appl);

    PD("Requesting flow allocation...\n");

    return (struct rl_kmsg_fa_resp_arrived *)
           rlite_issue_request(&appl->loop, RLITE_RMB(req),
                         sizeof(*req), 1, wait_ms, result);
}

int
rlite_flow_allocate_resp(struct rlite_appl *appl, uint32_t kevent_id,
                         uint16_t ipcp_id, uint16_t upper_ipcp_id,
                         uint32_t port_id, uint8_t response)
{
    struct rl_kmsg_fa_resp *req;
    struct rlite_msg_base *resp;
    int result;

    req = malloc(sizeof(*req));
    if (!req) {
        PE("Out of memory\n");
        return ENOMEM;
    }
    memset(req, 0, sizeof(*req));

    req->msg_type = RLITE_KER_FA_RESP;
    req->event_id = 1;
    req->kevent_id = kevent_id;
    req->ipcp_id = ipcp_id;  /* Currently unused by the kernel. */
    req->upper_ipcp_id = upper_ipcp_id;
    req->port_id = port_id;
    req->response = response;

    PD("Responding to flow allocation request...\n");

    resp = rlite_issue_request(&appl->loop, RLITE_RMB(req),
                         sizeof(*req), 0, 0, &result);
    assert(!resp);
    PD("result: %d\n", result);

    return result;
}

struct rl_kmsg_appl_register_resp *
rlite_appl_register(struct rlite_appl *appl, uint32_t event_id,
                    unsigned int wait_ms, int reg,
                    const struct rina_name *dif_name,
                    const struct rina_name *ipcp_name,
                    const struct rina_name *appl_name)
{
    struct rlite_ipcp *rlite_ipcp;

    rlite_ipcp = rlite_lookup_ipcp_by_name(&appl->loop, ipcp_name);
    if (!rlite_ipcp) {
        rlite_ipcp = rlite_select_ipcp_by_dif(&appl->loop, dif_name);
    }
    if (!rlite_ipcp) {
        PE("Could not find a suitable IPC process\n");
        return NULL;
    }

    /* Forward the request to the kernel. */
    return rlite_appl_register_req(appl, event_id, wait_ms,
                                   reg, rlite_ipcp->ipcp_id, appl_name);
}

int
rlite_appl_register_wait(struct rlite_appl *appl, int reg,
                         const struct rina_name *dif_name,
                         const struct rina_name *ipcp_name,
                         const struct rina_name *appl_name,
                         unsigned int wait_ms)
{
    struct rl_kmsg_appl_register_resp *resp;
    uint32_t event_id = rlite_evloop_get_id(&appl->loop);
    int ret = 0;

    resp = rlite_appl_register(appl, event_id, wait_ms, reg, dif_name,
                               ipcp_name, appl_name);

    if (!resp) {
        return -1;
    }

    if (resp->response != RLITE_SUCC) {
        ret = -1;
    }

    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                  RLITE_RMB(resp));
    free(resp);

    return ret;
}

int
rlite_flow_allocate(struct rlite_appl *appl, uint32_t event_id,
                    const struct rina_name *dif_name,
                    const struct rina_name *ipcp_name,
                    const struct rina_name *local_appl,
                    const struct rina_name *remote_appl,
                    const struct rlite_flow_spec *flowspec,
                    unsigned int *port_id, unsigned int wait_ms,
                    uint16_t upper_ipcp_id)
{
    struct rl_kmsg_fa_resp_arrived *kresp;
    struct rlite_ipcp *rlite_ipcp;
    int result;

    rlite_ipcp = rlite_lookup_ipcp_by_name(&appl->loop, ipcp_name);
    if (!rlite_ipcp) {
        rlite_ipcp = rlite_select_ipcp_by_dif(&appl->loop, dif_name);
    }
    if (!rlite_ipcp) {
        PE("No suitable IPCP found\n");
        return -1;
    }

    kresp = flow_allocate_req(appl, event_id, wait_ms,
                              rlite_ipcp->ipcp_id, upper_ipcp_id, local_appl,
                              remote_appl, flowspec, &result);
    if (!kresp) {
        if (wait_ms || result) {
            PE("Flow allocation request failed\n");
            return -1;
        }

        /* (wait_ms == 0 && result == 0) means non-blocking invocation, so
         * it is ok to get NULL. */
        *port_id = ~0U;

        return 0;
    }

    PI("Flow allocation response: ret = %u, port-id = %u\n",
                kresp->result, kresp->port_id);
    result = kresp->result;
    *port_id = kresp->port_id;
    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                  RLITE_RMB(kresp));
    free(kresp);

    return result;
}

struct rlite_pending_flow_req *
rlite_flow_req_wait(struct rlite_appl *appl)
{
    struct list_head *elem = NULL;

    pthread_mutex_lock(&appl->lock);
    while ((elem = list_pop_front(&appl->pending_flow_reqs)) == NULL) {
        pthread_cond_wait(&appl->flow_req_arrived_cond,
                          &appl->lock);
    }
    pthread_mutex_unlock(&appl->lock);

    return container_of(elem, struct rlite_pending_flow_req, node);
}

static int
open_port_common(uint32_t port_id, unsigned int mode, uint32_t ipcp_id)
{
    struct rlite_ioctl_info info;
    int fd;
    int ret;

    fd = open("/dev/rlite-io", O_RDWR);
    if (fd < 0) {
        perror("open(/dev/rlite-io)");
        return -1;
    }

    info.port_id = port_id;
    info.ipcp_id = ipcp_id;
    info.mode = mode;

    ret = ioctl(fd, 73, &info);
    if (ret) {
        perror("ioctl(/dev/rlite-io)");
        return -1;
    }

    return fd;
}

int
rlite_open_appl_port(uint32_t port_id)
{
    return open_port_common(port_id, RLITE_IO_MODE_APPL_BIND, 0);
}

int rlite_open_mgmt_port(uint16_t ipcp_id)
{
    /* The port_id argument is not valid in this call, it will not
     * be considered by the kernel. */
    return open_port_common(~0U, RLITE_IO_MODE_IPCP_MGMT, ipcp_id);
}

/* rlite_flow_allocate() + rlite_open_appl_port() */
int
rlite_flow_allocate_open(struct rlite_appl *appl,
                   const struct rina_name *dif_name,
                   const struct rina_name *ipcp_name,
                   const struct rina_name *local_appl,
                   const struct rina_name *remote_appl,
                   const struct rlite_flow_spec *flowspec,
                   unsigned int wait_ms)
{
    unsigned int port_id;
    uint32_t event_id;
    int ret;

    if (wait_ms == 0) {
        /* If the user wants to work in non-blocking mode, it
         * must use rlite_flow_allocate() directly. */
        PE("Cannot work in non-blocking mode\n");
        return -1;
    }

    event_id = rlite_evloop_get_id(&appl->loop);

    ret = rlite_flow_allocate(appl, event_id, dif_name, ipcp_name,
                              local_appl, remote_appl, flowspec,
                              &port_id, wait_ms, 0xffff);
    if (ret) {
        return -1;
    }

    return rlite_open_appl_port(port_id);
}

/* rlite_flow_req_wait() + rlite_open_appl_port() */
int
rlite_flow_req_wait_open(struct rlite_appl *appl)
{
    struct rlite_pending_flow_req *pfr;
    unsigned int port_id;
    int result;

    pfr = rlite_flow_req_wait(appl);
    PD("flow request arrived: [ipcp_id = %u, data_port_id = %u]\n",
            pfr->ipcp_id, pfr->port_id);

    /* Always accept incoming connection, for now. */
    result = rlite_flow_allocate_resp(appl, pfr->kevent_id,
                                      pfr->ipcp_id, 0xffff,
                                      pfr->port_id, 0);
    port_id = pfr->port_id;
    rlite_pending_flow_req_free(pfr);

    if (result) {
        return -1;
    }

    return rlite_open_appl_port(port_id);
}

int
rlite_appl_init(struct rlite_appl *appl)
{
    int ret;

    pthread_mutex_init(&appl->lock, NULL);
    pthread_cond_init(&appl->flow_req_arrived_cond, NULL);
    list_init(&appl->pending_flow_reqs);

    ret = rlite_evloop_init(&appl->loop, "/dev/rlite",
                            rlite_kernel_handlers);
    if (ret) {
        return ret;
    }

    return 0;
}

int
rlite_appl_fini(struct rlite_appl *appl)
{
    return rlite_evloop_fini(&appl->loop);
}
