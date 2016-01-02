#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/time.h>
#include <rina/rina-utils.h>

#include "application.h"


static void
client(int argc, char **argv, struct application *application)
{
    struct rina_name dif_name;
    struct rina_name this_application;
    struct rina_name remote_application;
    unsigned int port_id;
    struct timeval t_start, t_end;
    unsigned long us;
    int ret;
    int fd;
    char buf[4096];
    int size = 10;

    (void) argc;
    (void) argv;

    if (size > sizeof(buf)) {
        size = sizeof(buf);
    }

    ipcps_fetch(&application->loop);

    rina_name_fill(&dif_name, "d.DIF", "", "", "");
    rina_name_fill(&this_application, "client", "1", NULL, NULL);
    rina_name_fill(&remote_application, "server", "1", NULL, NULL);

    ret = flow_allocate(application, &dif_name, &this_application,
                        &remote_application, &port_id);
    if (ret) {
        return;
    }

    fd = open_port(port_id);
    if (fd < 0) {
        return;
    }

    memset(buf, 'x', size);

    gettimeofday(&t_start, NULL);

    ret = write(fd, buf, size);
    if (ret != size) {
        if (ret < 0) {
            perror("write(buf)");
        } else {
            printf("Partial write %d/%d\n", ret, size);
        }
    }

    ret = read(fd, buf, sizeof(buf));
    if (ret < 0) {
        perror("read(buf");
    }

    gettimeofday(&t_end, NULL);
    us = 1000000 * (t_end.tv_sec - t_start.tv_sec) +
            (t_end.tv_usec - t_start.tv_usec);

    printf("SDU size: %d bytes, latency: %lu us\n", ret, us);

    close(fd);
}

static int
server(int argc, char **argv, struct application *application)
{
    struct rina_name dif_name;
    struct rina_name this_application;
    struct pending_flow_req *pfr = NULL;
    int ret;

    ipcps_fetch(&application->loop);

    rina_name_fill(&dif_name, "d.DIF", "", "", "");
    rina_name_fill(&this_application, "server", "1", NULL, NULL);

    ret = application_register(application, 1, &dif_name, &this_application);
    if (ret) {
        return ret;
    }

    for (;;) {
        unsigned int port_id;
        int result;
        int fd;
        char buf[4096];
        int n, m;

        pfr = flow_request_wait(application);
        port_id = pfr->port_id;
        printf("%s: flow request arrived: [ipcp_id = %u, port_id = %u]\n",
                __func__, pfr->ipcp_id, pfr->port_id);

        /* Always accept incoming connection, for now. */
        result = flow_allocate_resp(application, pfr->ipcp_id,
                                    pfr->port_id, 0);
        free(pfr);

        if (result) {
            continue;
        }

        fd = open_port(port_id);
        if (fd < 0) {
            continue;
        }

        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            perror("read(flow)");
            goto clos;
        }

        m = write(fd, buf, n);
        if (m != n) {
            if (m < 0) {
                perror("write(flow)");
            } else {
                printf("partial write");
            }
        }
clos:
        close(fd);
    }

    return 0;
}

int
main(int argc, char **argv)
{
    struct application application;
    int ret;
    int opt;
    int listen = 0;
    const char *type = "echo";

    while ((opt = getopt(argc, argv, "lt:")) != -1) {
        switch (opt) {
            case 'l':
                listen = 1;
                break;

            case 't':
                type = optarg;
                break;

            default:
                printf("    Unrecognized option %c\n", opt);
                exit(EXIT_FAILURE);
        }
    }

    ret = rina_application_init(&application);
    if (ret) {
        return ret;
    }

    if (strcmp(type, "echo") == 0) {
        if (listen) {
            server(argc, argv, &application);
        } else {
            client(argc, argv, &application);
        }
    } else {
        printf("    Unknown test type %s\n", type);
    }

    return rina_application_fini(&application);
}
