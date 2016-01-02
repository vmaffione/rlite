#include <vector>
#include <list>
#include <map>
#include <string>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <stdint.h>
#include <cstdlib>
#include <cassert>

#include "rinalite/rinalite-common.h"
#include "rinalite/rinalite-utils.h"

#include "cdap.hpp"
#include "uipcp-container.h"

using namespace std;


static string dft_obj_name = "/dif/mgmt/fa/dft";
static string dft_obj_class = "dft";
static string whatevercast_obj_name = "/daf/mgmt/naming/whatevercast";

struct Neighbor {
    struct rina_name ipcp_name;
    int flow_fd;
    unsigned int port_id;

    Neighbor(const struct rina_name *name, int fd, unsigned int port_id);
    Neighbor(const Neighbor &other);
    ~Neighbor();
};

Neighbor::Neighbor(const struct rina_name *name, int fd, unsigned int port_id_)
{
    rina_name_copy(&ipcp_name, name);
    flow_fd = fd;
    port_id = port_id_;
}

Neighbor::Neighbor(const Neighbor& other)
{
    rina_name_copy(&ipcp_name, &other.ipcp_name);
    flow_fd = other.flow_fd;
    port_id = other.port_id;
}

Neighbor::~Neighbor()
{
    rina_name_free(&ipcp_name);
}

typedef int (*rib_handler_t)(struct uipcp_rib *);

struct uipcp_rib {
    /* Backpointer to parent data structure. */
    struct uipcp *uipcp;

    map< string, rib_handler_t > handlers;

    /* Neighbors. */
    list< Neighbor > neighbors;

    /* Directory Forwarding Table. */
    map< string, uint64_t > dft;

    uipcp_rib(struct uipcp *_u) : uipcp(_u) {}
};

static int
dft_handler(struct uipcp_rib *)
{
    return 0;
}

static int
whatevercast_handler(struct uipcp_rib *)
{
    return 0;
}

extern "C" struct uipcp_rib *
rib_create(struct uipcp *uipcp)
{
    struct uipcp_rib *rib = new uipcp_rib(uipcp);

    if (!rib) {
        return NULL;
    }

    /* Insert the handlers for the RIB objects. */

    rib->handlers.insert(make_pair(dft_obj_name, dft_handler));

    rib->handlers.insert(make_pair(whatevercast_obj_name,
                                   whatevercast_handler));

    return rib;
}

extern "C" void
rib_destroy(struct uipcp_rib *rib)
{
    int ret;

    for (list<Neighbor>::iterator neigh = rib->neighbors.begin();
                        neigh != rib->neighbors.end(); neigh++) {
        ret = close(neigh->flow_fd);
        if (ret) {
            PE("%s: Error deallocating N-1 flow fd %d\n", __func__,
               neigh->flow_fd);
        }
    }

    delete rib;
}

static int
rib_remote_sync(struct uipcp_rib *rib, bool create, const string& obj_class,
                const string& obj_name, int x)
{
    struct enrolled_neighbor *neigh;
#if 0
    CDAPConn conn(neigh->flow_fd, 1);
#endif
    CDAPMessage m;
    int invoke_id;

    list_for_each_entry(neigh, &rib->uipcp->enrolled_neighbors, node) {
        if (create) {
            m.m_create(gpb::F_NO_FLAGS, obj_class, obj_name,
                       0, 0, "");
        } else {
            m.m_delete(gpb::F_NO_FLAGS, obj_class, obj_name,
                       0, 0, "");
        }
    }

#if 0
    conn.msg_send(&m, 0);
#endif
}

extern "C" int
rib_neighbor_add(struct uipcp_rib *rib, const struct rina_name *neigh_name,
                 int neigh_fd, unsigned int neigh_port_id)
{
    /*
    CDAPAuthValue av;
    CDAPMessage m;

    m.m_connect(gpb::AUTH_NONE, &av, &local_appl, &neigh->ipcp_name);
    */

    rib->neighbors.push_back(Neighbor(neigh_name, neigh_fd, neigh_port_id));

    return 0;
}

extern "C" int
rib_application_register(struct uipcp_rib *rib, int reg,
                         const struct rina_name *appl_name)
{
    char *name_s = rina_name_to_string(appl_name);
    map< string, uint64_t >::iterator mit;
    struct uipcp *uipcp = rib->uipcp;
    uint64_t local_addr;
    string name_str;
    int ret;
    bool create = true;

    ret = rinalite_lookup_ipcp_addr_by_id(&uipcp->appl.loop,
                                          uipcp->ipcp_id,
                                          &local_addr);
    assert(!ret);

    if (!name_s) {
        PE("%s: Out of memory\n", __func__);
        return -1;
    }

    name_str = name_s;
    free(name_s);

    mit = rib->dft.find(name_str);

    if (reg) {
        if (mit != rib->dft.end()) {
            PE("%s: Application %s already registered on uipcp with address "
                    "[%llu], my address being [%llu]\n", __func__, name_str.c_str(),
                    (long long unsigned)mit->second, (long long unsigned)local_addr);
            return -1;
        }

        /* Insert the object into the RIB. */
        rib->dft.insert(make_pair(name_str, local_addr));

    } else {
        if (mit == rib->dft.end()) {
            PE("%s: Application %s was not registered here\n", __func__,
                name_str.c_str());
            return -1;
        }

        /* Remove the object from the RIB. */
        rib->dft.erase(mit);
        create = false;
    }

    rib_remote_sync(rib, create, dft_obj_class, dft_obj_name, 10329);

    PD("%s: Application %s %sregistered %s uipcp %d\n", __func__,
            name_str.c_str(), reg ? "" : "un", reg ? "to" : "from",
            uipcp->ipcp_id);

    return 0;
}
