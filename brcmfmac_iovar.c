/*
 * brcmfmac_iovar - Runtime iovar access for brcmfmac via nl80211 vendor commands
 *
 * Communicates with Broadcom/Cypress FullMAC firmware through the mainline
 * brcmfmac driver's nl80211 vendor command interface (BRCMF_VNDR_CMDS_DCMD).
 *
 * This bypasses the need for:
 *   - The proprietary Broadcom "wl" utility (licensing issues)
 *   - Custom kernel modules or driver patches
 *   - Module reload or WiFi disruption
 *
 * Mechanism:
 *   userspace (this tool)
 *       -> NL80211_CMD_VENDOR via generic netlink socket
 *       -> kernel cfg80211 routes to brcmfmac vendor.c
 *       -> brcmf_cfg80211_vndr_cmds_dcmd_handler()
 *       -> brcmf_fil_cmd_data_set() / brcmf_fil_cmd_data_get()
 *       -> BCDC protocol over SDIO/USB/PCIe bus
 *       -> CYW43xx firmware processes iovar
 *
 * Kernel source references:
 *   drivers/net/wireless/broadcom/brcm80211/brcmfmac/vendor.c
 *   drivers/net/wireless/broadcom/brcm80211/brcmfmac/vendor.h
 *   drivers/net/wireless/broadcom/brcm80211/brcmfmac/fwil.h
 *
 * Requirements:
 *   - libnl-3, libnl-genl-3 (runtime)
 *   - CAP_NET_ADMIN or root (required by nl80211)
 *   - brcmfmac driver loaded with active wireless interface
 *
 * Copyright (c) 2026 Volumio Community
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Build:
 *   gcc -Wall -O2 -o brcmfmac_iovar brcmfmac_iovar.c \
 *       $(pkg-config --cflags --libs libnl-3.0 libnl-genl-3.0)
 *
 * Usage:
 *   brcmfmac_iovar <interface> get_int <iovar_name>
 *   brcmfmac_iovar <interface> set_int <iovar_name> <value>
 *
 * Examples:
 *   brcmfmac_iovar wlan0 get_int btc_mode
 *   brcmfmac_iovar wlan0 set_int btc_mode 4
 *   brcmfmac_iovar wlan0 get_int btc_params
 */

/* Feature test macro - must be before any includes */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <netdb.h>

/*
 * Suppress warnings from libnl system headers.
 * netlink/addr.h has a struct addrinfo forward declaration issue.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <netlink/attr.h>
#pragma GCC diagnostic pop

#include <linux/nl80211.h>

/* -------------------------------------------------------------------------
 * Constants from kernel brcmfmac headers
 * Source: drivers/net/wireless/broadcom/brcm80211/brcmfmac/
 * ------------------------------------------------------------------------- */

/* Broadcom OUI used as nl80211 vendor ID */
/* vendor.c: #define BROADCOM_OUI 0x001018 */
#define BROADCOM_OUI        0x001018

/* Vendor subcommand for dongle command passthrough */
/* vendor.h: enum brcmf_vndr_cmds { BRCMF_VNDR_CMDS_DCMD = 1 } */
#define BRCMF_VNDR_CMDS_DCMD   1

/* Firmware interface layer command IDs */
/* fwil.h */
#define BRCMF_C_GET_VAR     262
#define BRCMF_C_SET_VAR     263

/* nl80211 vendor response attribute IDs */
/* vendor.h: enum brcmf_nlattrs */
#define BRCMF_NLATTR_LEN     1
#define BRCMF_NLATTR_DATA    2

/* -------------------------------------------------------------------------
 * Vendor command header - must match kernel struct brcmf_vndr_dcmd_hdr
 * Source: vendor.h
 *
 * NOTE: The kernel handler validates:
 *   - total data length >= sizeof(this header)
 *   - offset <= total data length
 * It does NOT validate the magic field.
 * ------------------------------------------------------------------------- */
struct brcmf_vndr_dcmd_hdr {
    uint32_t cmd;       /* dongle command (BRCMF_C_GET_VAR / BRCMF_C_SET_VAR) */
    int32_t  len;       /* length of expected return buffer */
    uint32_t offset;    /* byte offset where payload begins within vendor data */
    uint32_t set;       /* 0 = get, 1 = set */
    uint32_t magic;     /* not validated by mainline handler */
};

/* -------------------------------------------------------------------------
 * Callback state for receiving vendor command response
 * ------------------------------------------------------------------------- */
struct iovar_response {
    uint8_t *data;
    size_t   len;
    int      error;
};

/* -------------------------------------------------------------------------
 * nl80211 error handler - captures firmware/driver error codes
 * ------------------------------------------------------------------------- */
static int error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err,
                         void *arg)
{
    struct iovar_response *resp = arg;
    (void)nla;
    resp->error = err->error;
    return NL_STOP;
}

/* -------------------------------------------------------------------------
 * nl80211 finish handler - signals completion of dump/multipart
 * ------------------------------------------------------------------------- */
static int finish_handler(struct nl_msg *msg, void *arg)
{
    struct iovar_response *resp = arg;
    (void)msg;
    resp->error = 0;
    return NL_SKIP;
}

/* -------------------------------------------------------------------------
 * nl80211 ack handler - signals successful command completion
 * ------------------------------------------------------------------------- */
static int ack_handler(struct nl_msg *msg, void *arg)
{
    struct iovar_response *resp = arg;
    (void)msg;
    resp->error = 0;
    return NL_STOP;
}

/* -------------------------------------------------------------------------
 * nl80211 valid message handler - extracts vendor response data
 *
 * The kernel brcmfmac vendor handler returns response data as:
 *   NL80211_ATTR_VENDOR_DATA containing:
 *     BRCMF_NLATTR_DATA (2) = response bytes
 *     BRCMF_NLATTR_LEN  (1) = chunk length (if multi-part, not typical)
 * ------------------------------------------------------------------------- */
static int response_handler(struct nl_msg *msg, void *arg)
{
    struct iovar_response *resp = arg;
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    struct genlmsghdr *gnlh;
    struct nlattr *vendor_attr;
    int rem;

    gnlh = nlmsg_data(nlmsg_hdr(msg));

    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);

    if (!tb[NL80211_ATTR_VENDOR_DATA]) {
        resp->error = -ENODATA;
        return NL_SKIP;
    }

    /*
     * Parse nested vendor attributes within NL80211_ATTR_VENDOR_DATA.
     * BRCMF_NLATTR_DATA (2) contains the raw firmware response.
     */
    nla_for_each_nested(vendor_attr, tb[NL80211_ATTR_VENDOR_DATA], rem) {
        if (nla_type(vendor_attr) == BRCMF_NLATTR_DATA) {
            resp->len = (size_t)nla_len(vendor_attr);
            resp->data = malloc(resp->len);
            if (resp->data) {
                memcpy(resp->data, nla_data(vendor_attr), resp->len);
            } else {
                resp->error = -ENOMEM;
            }
            break;
        }
    }

    return NL_SKIP;
}

/* -------------------------------------------------------------------------
 * send_vendor_cmd - Send an nl80211 vendor command to brcmfmac
 *
 * Constructs and sends NL80211_CMD_VENDOR with:
 *   NL80211_ATTR_IFINDEX       = interface index
 *   NL80211_ATTR_VENDOR_ID     = BROADCOM_OUI (0x001018)
 *   NL80211_ATTR_VENDOR_SUBCMD = BRCMF_VNDR_CMDS_DCMD (1)
 *   NL80211_ATTR_VENDOR_DATA   = packed header + iovar payload
 *
 * Parameters:
 *   ifindex  - network interface index (from if_nametoindex)
 *   cmd      - BRCMF_C_GET_VAR (262) or BRCMF_C_SET_VAR (263)
 *   is_set   - 0 for get, 1 for set
 *   payload  - iovar name + optional value (already packed by caller)
 *   payload_len - length of payload
 *   ret_len  - expected return buffer length
 *   resp     - output: response data and error code
 *
 * Returns: 0 on success, negative errno on failure
 * ------------------------------------------------------------------------- */
static int send_vendor_cmd(int ifindex, uint32_t cmd, int is_set,
                           const uint8_t *payload, size_t payload_len,
                           int32_t ret_len, struct iovar_response *resp)
{
    struct nl_sock *sk = NULL;
    struct nl_msg *msg = NULL;
    struct nl_cb *cb = NULL;
    int nl80211_id;
    int ret = -1;
    struct brcmf_vndr_dcmd_hdr hdr;
    uint8_t *vendor_data = NULL;
    size_t vendor_data_len;

    /* Initialise response */
    memset(resp, 0, sizeof(*resp));
    resp->error = -EINPROGRESS;

    /* Allocate netlink socket */
    sk = nl_socket_alloc();
    if (!sk) {
        fprintf(stderr, "ERROR: Failed to allocate netlink socket\n");
        return -ENOMEM;
    }

    /* Connect to generic netlink */
    ret = genl_connect(sk);
    if (ret < 0) {
        fprintf(stderr, "ERROR: Failed to connect to generic netlink: %s\n",
                nl_geterror(ret));
        goto out;
    }

    /* Resolve nl80211 family ID */
    nl80211_id = genl_ctrl_resolve(sk, "nl80211");
    if (nl80211_id < 0) {
        fprintf(stderr, "ERROR: nl80211 not found (is cfg80211 loaded?)\n");
        ret = nl80211_id;
        goto out;
    }

    /* Build the vendor data blob:
     *   [brcmf_vndr_dcmd_hdr][payload...]
     *
     * The header's offset field points to where payload begins
     * within the entire vendor data blob.
     */
    vendor_data_len = sizeof(hdr) + payload_len;
    vendor_data = calloc(1, vendor_data_len);
    if (!vendor_data) {
        ret = -ENOMEM;
        goto out;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.cmd    = cmd;
    hdr.len    = ret_len;
    hdr.offset = sizeof(hdr);  /* payload starts right after header */
    hdr.set    = is_set ? 1 : 0;
    hdr.magic  = 0;            /* not validated by mainline handler */

    memcpy(vendor_data, &hdr, sizeof(hdr));
    memcpy(vendor_data + sizeof(hdr), payload, payload_len);

    /* Allocate nl80211 message */
    msg = nlmsg_alloc();
    if (!msg) {
        fprintf(stderr, "ERROR: Failed to allocate netlink message\n");
        ret = -ENOMEM;
        goto out;
    }

    /* Populate NL80211_CMD_VENDOR */
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0,
                0, NL80211_CMD_VENDOR, 0);

    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);
    nla_put_u32(msg, NL80211_ATTR_VENDOR_ID, BROADCOM_OUI);
    nla_put_u32(msg, NL80211_ATTR_VENDOR_SUBCMD, BRCMF_VNDR_CMDS_DCMD);
    nla_put(msg, NL80211_ATTR_VENDOR_DATA, vendor_data_len, vendor_data);

    /* Set up callback handlers */
    cb = nl_cb_alloc(NL_CB_DEFAULT);
    if (!cb) {
        ret = -ENOMEM;
        goto out;
    }

    nl_cb_err(cb, NL_CB_CUSTOM, error_handler, resp);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, resp);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, resp);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, response_handler, resp);

    /* Send and receive */
    ret = nl_send_auto(sk, msg);
    if (ret < 0) {
        fprintf(stderr, "ERROR: Failed to send netlink message: %s\n",
                nl_geterror(ret));
        goto out;
    }

    /* Process response(s) until completion */
    while (resp->error == -EINPROGRESS) {
        nl_recvmsgs(sk, cb);
    }

    ret = resp->error;

out:
    if (cb)
        nl_cb_put(cb);
    if (msg)
        nlmsg_free(msg);
    free(vendor_data);
    if (sk)
        nl_socket_free(sk);
    return ret;
}

/* -------------------------------------------------------------------------
 * get_iovar_int - Read a 32-bit integer iovar from firmware
 *
 * For GET_VAR, the payload is the null-terminated iovar name.
 * The firmware replaces the buffer contents with the value.
 * ret_len must be large enough for the iovar name AND the response.
 * ------------------------------------------------------------------------- */
static int get_iovar_int(int ifindex, const char *iovar, uint32_t *value)
{
    struct iovar_response resp;
    size_t name_len = strlen(iovar) + 1; /* include null terminator */
    /* ret_len: firmware needs space to read name AND write response.
     * For integer iovars, the response overwrites the name buffer.
     * We need at least max(name_len, sizeof(uint32_t)). Add margin. */
    int32_t ret_len = name_len > 256 ? name_len + 4 : 256;
    int ret;

    ret = send_vendor_cmd(ifindex, BRCMF_C_GET_VAR, 0,
                          (const uint8_t *)iovar, name_len,
                          ret_len, &resp);

    if (ret != 0) {
        fprintf(stderr, "ERROR: GET_VAR '%s' failed: %d (%s)\n",
                iovar, ret, strerror(-ret));
        return ret;
    }

    if (resp.data && resp.len >= sizeof(uint32_t)) {
        memcpy(value, resp.data, sizeof(uint32_t));
        /* Value is little-endian from firmware - host byte order on ARM */
        free(resp.data);
        return 0;
    }

    fprintf(stderr, "ERROR: GET_VAR '%s' returned insufficient data "
            "(got %zu bytes, need %zu)\n", iovar, resp.len, sizeof(uint32_t));
    free(resp.data);
    return -ENODATA;
}

/* -------------------------------------------------------------------------
 * set_iovar_int - Write a 32-bit integer iovar to firmware
 *
 * For SET_VAR, the payload is:
 *   [iovar_name\0][uint32_t value]
 * The firmware reads the name, then the value following the null terminator.
 * ------------------------------------------------------------------------- */
static int set_iovar_int(int ifindex, const char *iovar, uint32_t value)
{
    struct iovar_response resp;
    size_t name_len = strlen(iovar) + 1;
    size_t payload_len = name_len + sizeof(uint32_t);
    uint8_t *payload;
    int ret;

    payload = calloc(1, payload_len);
    if (!payload)
        return -ENOMEM;

    /* Pack: "iovar_name\0" + le32(value) */
    memcpy(payload, iovar, name_len);
    memcpy(payload + name_len, &value, sizeof(uint32_t));

    ret = send_vendor_cmd(ifindex, BRCMF_C_SET_VAR, 1,
                          payload, payload_len,
                          (int32_t)payload_len, &resp);

    free(payload);
    free(resp.data);

    if (ret != 0) {
        fprintf(stderr, "ERROR: SET_VAR '%s' = %u failed: %d (%s)\n",
                iovar, value, ret, strerror(-ret));
    }

    return ret;
}

/* -------------------------------------------------------------------------
 * Usage and main
 * ------------------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
        "brcmfmac_iovar - Runtime iovar access via nl80211 vendor commands\n"
        "\n"
        "Usage:\n"
        "  %s <interface> get_int <iovar>\n"
        "  %s <interface> set_int <iovar> <value>\n"
        "\n"
        "Examples:\n"
        "  %s wlan0 get_int btc_mode          Read BT coexistence mode\n"
        "  %s wlan0 set_int btc_mode 4        Set BT coex to full TDM\n"
        "  %s wlan0 get_int btc_params        Read BT coex parameters\n"
        "\n"
        "Known btc_mode values:\n"
        "  0 = disabled\n"
        "  1 = default (basic coexistence)\n"
        "  2 = serial (SECI-based)\n"
        "  4 = full TDM (time-division multiplexing, recommended for A2DP)\n"
        "\n"
        "Requires: root or CAP_NET_ADMIN\n"
        "Driver:   brcmfmac (mainline kernel, no patches needed)\n"
        "\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    const char *ifname;
    const char *command;
    const char *iovar;
    int ifindex;

    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    ifname  = argv[1];
    command = argv[2];
    iovar   = argv[3];

    /* Resolve interface name to index */
    ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "ERROR: Interface '%s' not found: %s\n",
                ifname, strerror(errno));
        return 1;
    }

    if (strcmp(command, "get_int") == 0) {
        uint32_t value;
        int ret = get_iovar_int(ifindex, iovar, &value);
        if (ret == 0) {
            printf("%s = %u\n", iovar, value);
            return 0;
        }
        return 1;
    }

    if (strcmp(command, "set_int") == 0) {
        if (argc < 5) {
            fprintf(stderr, "ERROR: set_int requires a value argument\n");
            usage(argv[0]);
            return 1;
        }
        uint32_t value = (uint32_t)strtoul(argv[4], NULL, 0);
        int ret = set_iovar_int(ifindex, iovar, value);
        if (ret == 0) {
            printf("%s set to %u\n", iovar, value);
            return 0;
        }
        return 1;
    }

    fprintf(stderr, "ERROR: Unknown command '%s'\n", command);
    usage(argv[0]);
    return 1;
}
