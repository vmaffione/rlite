/*
 * RLITE shim DIF for HV
 *
 *    Vincenzo Maffione <v.maffione@gmail.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/types.h>
#include <linux/string.h>
#include "rlite/utils.h"
#include <vmpi-provider.h>
#include "rlite-kernel.h"
#include "shim-hv-msg.h"

#include <linux/module.h>
#include <linux/uio.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/workqueue.h>


struct rlite_shim_hv {
    struct ipcp_entry *ipcp;
    struct vmpi_ops vmpi_ops;
    unsigned int vmpi_id;
};

static int
shim_hv_send_ctrl_msg(struct ipcp_entry *ipcp,
                      struct rlite_msg_base *msg)
{
    struct rlite_shim_hv *priv = (struct rlite_shim_hv *)ipcp->priv;
    struct iovec iov;
    unsigned int serlen;
    int ret;
    uint8_t *serbuf;

    serlen = rlite_msg_serlen(rlite_shim_hv_numtables, RLITE_SHIM_HV_MSG_MAX,
                             msg);
    serbuf = kmalloc(serlen, GFP_ATOMIC);
    if (!serbuf) {
        PE("Out of memory\n");
        return -ENOMEM;
    }

    ret = serialize_rlite_msg(rlite_shim_hv_numtables, RLITE_SHIM_HV_MSG_MAX,
                             serbuf, msg);
    if (ret != serlen) {
        PE("Error while serializing\n");
        return -EINVAL;
    }

    iov.iov_base = serbuf;
    iov.iov_len = serlen;
    ret = priv->vmpi_ops.write(&priv->vmpi_ops, 0, &iov, 1);

    kfree(serbuf);

    return !(ret == serlen);
}

static void
shim_hv_handle_ctrl_message(struct rlite_shim_hv *priv,
                            const char *serbuf, int serlen)
{
    int ret;

    rlite_msg_t ty = *((const rlite_msg_t *)serbuf);

    if (ty == RLITE_SHIM_HV_FA_REQ) {
        struct rlite_hmsg_fa_req req;

        ret = deserialize_rlite_msg(rlite_shim_hv_numtables,
                                   RLITE_SHIM_HV_MSG_MAX, serbuf, serlen,
                                   &req, sizeof(req));
        if (ret) {
            goto des_fail;
        }

        ret = rlite_fa_req_arrived(priv->ipcp, 0, req.local_port, 0, 0,
                    &req.remote_appl, &req.local_appl, NULL);
        if (ret) {
            PE("failed to report flow allocation request\n");
        }
    } else if (ty == RLITE_SHIM_HV_FA_RESP) {
        struct rlite_hmsg_fa_resp resp;

        ret = deserialize_rlite_msg(rlite_shim_hv_numtables,
                                   RLITE_SHIM_HV_MSG_MAX, serbuf, serlen,
                                   &resp, sizeof(resp));
        if (ret) {
            goto des_fail;
        }

        /* XXX shouldn't we swap resp.remote_port and resp.local_port
         * arguments? */
        ret = rlite_fa_resp_arrived(priv->ipcp, resp.remote_port,
                                   resp.local_port, 0, 0, resp.response, NULL);
        if (ret) {
            PE("failed to report flow allocation response\n");
        }
    } else {
        PE("unknown ctrl msg type %u\n", ty);
    }

    return;

des_fail:
    if (ret) {
        PE("failed to deserialize msg type %u\n", ty);
    }
}

static void
shim_hv_read_callback(void *opaque, unsigned int channel,
                      const char *buf, int len)
{
    struct rlite_shim_hv *priv = (struct rlite_shim_hv *)opaque;
    struct rlite_buf *rb;

    if (unlikely(channel == 0)) {
        /* Control message. */
        shim_hv_handle_ctrl_message(priv, buf, len);
        return;
    }

    rb = rlite_buf_alloc(len, priv->ipcp->depth, GFP_ATOMIC);
    if (!rb) {
        PE("Out of memory\n");
        return;
    }

    memcpy(RLITE_BUF_DATA(rb), buf, len);

    rlite_sdu_rx(priv->ipcp, rb, channel - 1);
}

static void *
rlite_shim_hv_create(struct ipcp_entry *ipcp)
{
    struct rlite_shim_hv *priv;

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        return NULL;
    }

    priv->ipcp = ipcp;
    priv->vmpi_id = ~0U;

    PD("New IPC created [%p]\n", priv);

    return priv;
}

static void
rlite_shim_hv_destroy(struct ipcp_entry *ipcp)
{
    struct rlite_shim_hv *priv = ipcp->priv;

    kfree(priv);

    PD("IPC [%p] destroyed\n", priv);
}

static int
rlite_shim_hv_fa_req(struct ipcp_entry *ipcp,
                               struct flow_entry *flow)
{
    struct rlite_hmsg_fa_req req;

    req.msg_type = RLITE_SHIM_HV_FA_REQ;
    req.event_id = 0;
    rina_name_copy(&req.local_appl, &flow->local_appl);
    rina_name_copy(&req.remote_appl, &flow->remote_appl);
    req.local_port = flow->local_port;

    return shim_hv_send_ctrl_msg(ipcp, (struct rlite_msg_base *)&req);
}

static int
rlite_shim_hv_fa_resp(struct ipcp_entry *ipcp,
                                   struct flow_entry *flow,
                                   uint8_t response)
{
    struct rlite_hmsg_fa_resp resp;

    resp.msg_type = RLITE_SHIM_HV_FA_RESP;
    resp.event_id = 0;
    resp.local_port = flow->local_port;
    resp.remote_port = flow->remote_port;
    resp.response = response;

    return shim_hv_send_ctrl_msg(ipcp, (struct rlite_msg_base *)&resp);
}

static int
rlite_shim_hv_sdu_write(struct ipcp_entry *ipcp,
                       struct flow_entry *flow,
                       struct rlite_buf *rb, bool maysleep)
{
    struct iovec iov;
    struct rlite_shim_hv *priv = (struct rlite_shim_hv *)ipcp->priv;
    struct vmpi_ops *vmpi_ops = &priv->vmpi_ops;
    ssize_t ret;

    if (unlikely(!vmpi_ops->write)) {
        return -ENXIO;
    }

    if (unlikely(rb->len > PAGE_SIZE-8)) {
        RPD(5, "Exceeding maximum VMPI payload (%lu)\n",
            PAGE_SIZE-8);
        return -EINVAL;
    }

    iov.iov_base = RLITE_BUF_DATA(rb);
    iov.iov_len = rb->len;

    ret = vmpi_ops->write(vmpi_ops, flow->remote_port + 1,
                          &iov, 1);
    rlite_buf_free(rb);

    if (unlikely(ret != iov.iov_len)) {
        return ret < 0 ? ret : -ENOBUFS;
    }

    return 0;
}

static int
rlite_shim_hv_config(struct ipcp_entry *ipcp,
                    const char *param_name,
                    const char *param_value)
{
    struct rlite_shim_hv *priv = (struct rlite_shim_hv *)ipcp->priv;
    int ret = -EINVAL;

    if (strcmp(param_name, "vmpi-id") == 0) {
        unsigned int provider = VMPI_PROVIDER_AUTO;

        ret = kstrtouint(param_value, 10, &priv->vmpi_id);
        if (ret == 0) {
            PI("vmpi id set to %u\n", priv->vmpi_id);
        }

        ret = vmpi_provider_find_instance(provider, priv->vmpi_id, &priv->vmpi_ops);
        if (ret) {
            PE("vmpi_provider_find(%u, %u) failed\n", provider, priv->vmpi_id);
            return ret;
        }

        ret = priv->vmpi_ops.register_read_callback(&priv->vmpi_ops,
                shim_hv_read_callback, priv);
        if (ret) {
            PE("register_read_callback() failed\n");
            return ret;
        }
    }

    return ret;
}

#define SHIM_DIF_TYPE   "shim-hv"

static struct ipcp_factory shim_hv_factory = {
    .owner = THIS_MODULE,
    .dif_type = SHIM_DIF_TYPE,
    .use_cep_ids = false,
    .create = rlite_shim_hv_create,
    .ops.destroy = rlite_shim_hv_destroy,
    .ops.flow_allocate_req = rlite_shim_hv_fa_req,
    .ops.flow_allocate_resp = rlite_shim_hv_fa_resp,
    .ops.sdu_write = rlite_shim_hv_sdu_write,
    .ops.config = rlite_shim_hv_config,
};

static int __init
rlite_shim_hv_init(void)
{
    return rlite_ipcp_factory_register(&shim_hv_factory);
}

static void __exit
rlite_shim_hv_fini(void)
{
    rlite_ipcp_factory_unregister(SHIM_DIF_TYPE);
}

module_init(rlite_shim_hv_init);
module_exit(rlite_shim_hv_fini);
MODULE_LICENSE("GPL");
