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
#include <signal.h>
#include <assert.h>

#include <rina/rina-application-msg.h>
#include "helpers.h"


#define UNIX_DOMAIN_SOCKNAME    "/home/vmaffione/unix"

static void usage_and_quit(void);

static int
ipcm_connect()
{
    struct sockaddr_un server_address;
    int ret;
    int sfd;

    /* Open a Unix domain socket towards the IPCM. */
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket(AF_UNIX)");
        exit(EXIT_FAILURE);
    }
    memset(&server_address, 0, sizeof(server_address));
    server_address.sun_family = AF_UNIX;
    strncpy(server_address.sun_path, UNIX_DOMAIN_SOCKNAME,
            sizeof(server_address.sun_path) - 1);
    ret = connect(sfd, (struct sockaddr *)&server_address,
                    sizeof(server_address));
    if (ret) {
        perror("bind(AF_UNIX, path)");
        exit(EXIT_FAILURE);
        return -1;
    }

    return sfd;
}

static int ipcm_disconnect(int sfd)
{
        return close(sfd);
}

static int
read_response(int sfd)
{
    struct rina_msg_base_resp *resp;
    char msgbuf[4096];
    char serbuf[4096];
    int ret;
    int n;

    n = read(sfd, serbuf, sizeof(serbuf));
    if (n < 0) {
        printf("%s: read() error [%d]\n", __func__, n);
        return -1;
    }

    ret = deserialize_rina_msg(rina_application_numtables, serbuf,
                               n, msgbuf, sizeof(msgbuf));
    if (ret) {
        printf("%s: error while deserializing response [%d]\n",
                __func__, ret);
        return -1;
    }

    resp = (struct rina_msg_base_resp *)msgbuf;
    ret = (resp->result) == 0 ? 0 : -1;

    printf("IPCM response [type=%u] --> %d\n", resp->msg_type, ret);

    return ret;
}

static int application_register_common(const struct rina_name *app_name,
                                       const char *dif_name, int reg)
{
    struct rina_amsg_register msg;
    int fd;
    int ret;

    msg.msg_type = reg ? RINA_APPL_REGISTER : RINA_APPL_UNREGISTER;
    msg.event_id = 0;
    rina_name_copy(&msg.application_name, app_name);
    rina_name_fill(&msg.dif_name, dif_name, NULL, NULL, NULL);

    if (!rina_name_valid(&msg.application_name)) {
        printf("%s: Invalid application name\n", __func__);
        return -1;
    }

    if (!rina_name_valid(&msg.dif_name)) {
        printf("%s: Invalid dif name\n", __func__);
        return -1;
    }

    fd = ipcm_connect();
    if (fd < 0) {
        return fd;
    }

    ret = rina_msg_write(fd, (struct rina_msg_base *)&msg);
    if (ret) {
        return ret;
    }

    ret = read_response(fd);
    if (ret) {
        return ret;
    }

    return ipcm_disconnect(fd);
}

static int
application_register(int argc, char **argv)
{
    struct rina_name application_name;
    const char *dif_name;
    const char *ipcp_apn;
    const char *ipcp_api;

    if (argc < 3) {
        usage_and_quit();
    }

    dif_name = argv[0];
    ipcp_apn = argv[1];
    ipcp_api = argv[2];

    rina_name_fill(&application_name, ipcp_apn, ipcp_api, NULL, NULL);

    return application_register_common(&application_name, dif_name, 1);
}

static int
application_unregister(int argc, char **argv)
{
    struct rina_name application_name;
    const char *dif_name;
    const char *ipcp_apn;
    const char *ipcp_api;

    if (argc < 3) {
        usage_and_quit();
    }

    dif_name = argv[0];
    ipcp_apn = argv[1];
    ipcp_api = argv[2];

    rina_name_fill(&application_name, ipcp_apn, ipcp_api, NULL, NULL);

    return application_register_common(&application_name, dif_name, 0);
}

struct cmd_descriptor {
    const char *name;
    const char *usage;
    int (*func)(int argc, char **argv);
};

static struct cmd_descriptor cmd_descriptors[] = {
    {
        .name = "application-register",
        .usage = "DIF_NAME IPCP_APN IPCP_API",
        .func = application_register,
    },
    {
        .name = "application-unregister",
        .usage = "DIF_NAME IPCP_APN IPCP_API",
        .func = application_unregister,
    },
    {
        .name = "ipcp-create",
        .usage = "DIF_TYPE IPCP_APN IPCP_API",
        .func = NULL,
    },
    {
        .name = "ipcp-destroy",
        .usage = "IPCP_APN IPCP_API",
        .func = NULL,
    },
};

#define NUM_COMMANDS    (sizeof(cmd_descriptors)/sizeof(struct cmd_descriptor))

static void
usage_and_quit(void)
{
    int i;

    printf("Available commands:\n");

    for (i = 0; i < NUM_COMMANDS; i++) {
        printf("    %s %s\n", cmd_descriptors[i].name, cmd_descriptors[i].usage);
    }

    exit(EXIT_SUCCESS);
}

static int
process_args(int argc, char **argv)
{
    const char *cmd;
    int i;

    if (argc < 2) {
        /* No command. */
        usage_and_quit();
    }

    cmd = argv[1];

    for (i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(cmd, cmd_descriptors[i].name) == 0) {
            assert(cmd_descriptors[i].func);
            return cmd_descriptors[i].func(argc - 2, argv + 2);
        }
    }

    printf("Unknown command '%s'\n", cmd);
    usage_and_quit();

    return 0;
}

static void
sigint_handler(int signum)
{
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    int ret;

    /* Set an handler for SIGINT and SIGTERM so that we can remove
     * the Unix domain socket used to access the IPCM server. */
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ret = sigaction(SIGINT, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }
    ret = sigaction(SIGTERM, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGTERM)");
        exit(EXIT_FAILURE);
    }

    return process_args(argc, argv);
}
