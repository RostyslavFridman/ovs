/*
 * Copyright (c) 2009, 2010, 2011, 2012, 2013, 2014 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include <arpa/inet.h>
#include "odp-util.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <stdlib.h>
#include <string.h>
#include "byte-order.h"
#include "coverage.h"
#include "dpif.h"
#include "dynamic-string.h"
#include "flow.h"
#include "netlink.h"
#include "ofpbuf.h"
#include "packets.h"
#include "simap.h"
#include "timeval.h"
#include "util.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(odp_util);

/* The interface between userspace and kernel uses an "OVS_*" prefix.
 * Since this is fairly non-specific for the OVS userspace components,
 * "ODP_*" (Open vSwitch Datapath) is used as the prefix for
 * interactions with the datapath.
 */

/* The set of characters that may separate one action or one key attribute
 * from another. */
static const char *delimiters = ", \t\r\n";

static int parse_odp_key_mask_attr(const char *, const struct simap *port_names,
                              struct ofpbuf *, struct ofpbuf *);
static void format_odp_key_attr(const struct nlattr *a,
                                const struct nlattr *ma,
                                const struct hmap *portno_names, struct ds *ds,
                                bool verbose);

/* Returns one the following for the action with the given OVS_ACTION_ATTR_*
 * 'type':
 *
 *   - For an action whose argument has a fixed length, returned that
 *     nonnegative length in bytes.
 *
 *   - For an action with a variable-length argument, returns -2.
 *
 *   - For an invalid 'type', returns -1. */
static int
odp_action_len(uint16_t type)
{
    if (type > OVS_ACTION_ATTR_MAX) {
        return -1;
    }

    switch ((enum ovs_action_attr) type) {
    case OVS_ACTION_ATTR_OUTPUT: return sizeof(uint32_t);
    case OVS_ACTION_ATTR_USERSPACE: return -2;
    case OVS_ACTION_ATTR_PUSH_VLAN: return sizeof(struct ovs_action_push_vlan);
    case OVS_ACTION_ATTR_POP_VLAN: return 0;
    case OVS_ACTION_ATTR_PUSH_MPLS: return sizeof(struct ovs_action_push_mpls);
    case OVS_ACTION_ATTR_POP_MPLS: return sizeof(ovs_be16);
    case OVS_ACTION_ATTR_RECIRC: return sizeof(uint32_t);
    case OVS_ACTION_ATTR_HASH: return sizeof(struct ovs_action_hash);
    case OVS_ACTION_ATTR_SET: return -2;
    case OVS_ACTION_ATTR_SET_MASKED: return -2;
    case OVS_ACTION_ATTR_SAMPLE: return -2;

    case OVS_ACTION_ATTR_UNSPEC:
    case __OVS_ACTION_ATTR_MAX:
        return -1;
    }

    return -1;
}

/* Returns a string form of 'attr'.  The return value is either a statically
 * allocated constant string or the 'bufsize'-byte buffer 'namebuf'.  'bufsize'
 * should be at least OVS_KEY_ATTR_BUFSIZE. */
enum { OVS_KEY_ATTR_BUFSIZE = 3 + INT_STRLEN(unsigned int) + 1 };
static const char *
ovs_key_attr_to_string(enum ovs_key_attr attr, char *namebuf, size_t bufsize)
{
    switch (attr) {
    case OVS_KEY_ATTR_UNSPEC: return "unspec";
    case OVS_KEY_ATTR_ENCAP: return "encap";
    case OVS_KEY_ATTR_PRIORITY: return "skb_priority";
    case OVS_KEY_ATTR_SKB_MARK: return "skb_mark";
    case OVS_KEY_ATTR_TUNNEL: return "tunnel";
    case OVS_KEY_ATTR_IN_PORT: return "in_port";
    case OVS_KEY_ATTR_ETHERNET: return "eth";
    case OVS_KEY_ATTR_VLAN: return "vlan";
    case OVS_KEY_ATTR_ETHERTYPE: return "eth_type";
    case OVS_KEY_ATTR_IPV4: return "ipv4";
    case OVS_KEY_ATTR_IPV6: return "ipv6";
    case OVS_KEY_ATTR_TCP: return "tcp";
    case OVS_KEY_ATTR_TCP_FLAGS: return "tcp_flags";
    case OVS_KEY_ATTR_UDP: return "udp";
    case OVS_KEY_ATTR_SCTP: return "sctp";
    case OVS_KEY_ATTR_ICMP: return "icmp";
    case OVS_KEY_ATTR_ICMPV6: return "icmpv6";
    case OVS_KEY_ATTR_ARP: return "arp";
    case OVS_KEY_ATTR_ND: return "nd";
    case OVS_KEY_ATTR_MPLS: return "mpls";
    case OVS_KEY_ATTR_DP_HASH: return "dp_hash";
    case OVS_KEY_ATTR_RECIRC_ID: return "recirc_id";

    case __OVS_KEY_ATTR_MAX:
    default:
        snprintf(namebuf, bufsize, "key%u", (unsigned int) attr);
        return namebuf;
    }
}

static void
format_generic_odp_action(struct ds *ds, const struct nlattr *a)
{
    size_t len = nl_attr_get_size(a);

    ds_put_format(ds, "action%"PRId16, nl_attr_type(a));
    if (len) {
        const uint8_t *unspec;
        unsigned int i;

        unspec = nl_attr_get(a);
        for (i = 0; i < len; i++) {
            ds_put_char(ds, i ? ' ': '(');
            ds_put_format(ds, "%02x", unspec[i]);
        }
        ds_put_char(ds, ')');
    }
}

static void
format_odp_sample_action(struct ds *ds, const struct nlattr *attr)
{
    static const struct nl_policy ovs_sample_policy[] = {
        [OVS_SAMPLE_ATTR_PROBABILITY] = { .type = NL_A_U32 },
        [OVS_SAMPLE_ATTR_ACTIONS] = { .type = NL_A_NESTED }
    };
    struct nlattr *a[ARRAY_SIZE(ovs_sample_policy)];
    double percentage;
    const struct nlattr *nla_acts;
    int len;

    ds_put_cstr(ds, "sample");

    if (!nl_parse_nested(attr, ovs_sample_policy, a, ARRAY_SIZE(a))) {
        ds_put_cstr(ds, "(error)");
        return;
    }

    percentage = (100.0 * nl_attr_get_u32(a[OVS_SAMPLE_ATTR_PROBABILITY])) /
                        UINT32_MAX;

    ds_put_format(ds, "(sample=%.1f%%,", percentage);

    ds_put_cstr(ds, "actions(");
    nla_acts = nl_attr_get(a[OVS_SAMPLE_ATTR_ACTIONS]);
    len = nl_attr_get_size(a[OVS_SAMPLE_ATTR_ACTIONS]);
    format_odp_actions(ds, nla_acts, len);
    ds_put_format(ds, "))");
}

static const char *
slow_path_reason_to_string(uint32_t reason)
{
    switch ((enum slow_path_reason) reason) {
#define SPR(ENUM, STRING, EXPLANATION) case ENUM: return STRING;
        SLOW_PATH_REASONS
#undef SPR
    }

    return NULL;
}

const char *
slow_path_reason_to_explanation(enum slow_path_reason reason)
{
    switch (reason) {
#define SPR(ENUM, STRING, EXPLANATION) case ENUM: return EXPLANATION;
        SLOW_PATH_REASONS
#undef SPR
    }

    return "<unknown>";
}

static int
parse_flags(const char *s, const char *(*bit_to_string)(uint32_t),
            uint32_t *res_flags, uint32_t allowed, uint32_t *res_mask)
{
    uint32_t result = 0;
    int n;

    /* Parse masked flags in numeric format? */
    if (res_mask && ovs_scan(s, "%"SCNi32"/%"SCNi32"%n",
                             res_flags, res_mask, &n) && n > 0) {
        if (*res_flags & ~allowed || *res_mask & ~allowed) {
            return -EINVAL;
        }
        return n;
    }

    n = 0;

    if (res_mask && (*s == '+' || *s == '-')) {
        uint32_t flags = 0, mask = 0;

        /* Parse masked flags. */
        while (s[n] != ')') {
            bool set;
            uint32_t bit;
            int name_len;

            if (s[n] == '+') {
                set = true;
            } else if (s[n] == '-') {
                set = false;
            } else {
                return -EINVAL;
            }
            n++;

            name_len = strcspn(s + n, "+-)");

            for (bit = 1; bit; bit <<= 1) {
                const char *fname = bit_to_string(bit);
                size_t len;

                if (!fname) {
                    continue;
                }

                len = strlen(fname);
                if (len != name_len) {
                    continue;
                }
                if (!strncmp(s + n, fname, len)) {
                    if (mask & bit) {
                        /* bit already set. */
                        return -EINVAL;
                    }
                    if (!(bit & allowed)) {
                        return -EINVAL;
                    }
                    if (set) {
                        flags |= bit;
                    }
                    mask |= bit;
                    break;
                }
            }

            if (!bit) {
                return -EINVAL; /* Unknown flag name */
            }
            s += name_len;
        }

        *res_flags = flags;
        *res_mask = mask;
        return n;
    }

    /* Parse unmasked flags.  If a flag is present, it is set, otherwise
     * it is not set. */
    while (s[n] != ')') {
        unsigned long long int flags;
        uint32_t bit;
        int n0;

        if (ovs_scan(&s[n], "%lli%n", &flags, &n0)) {
            if (flags & ~allowed) {
                return -EINVAL;
            }
            n += n0 + (s[n + n0] == ',');
            result |= flags;
            continue;
        }

        for (bit = 1; bit; bit <<= 1) {
            const char *name = bit_to_string(bit);
            size_t len;

            if (!name) {
                continue;
            }

            len = strlen(name);
            if (!strncmp(s + n, name, len) &&
                (s[n + len] == ',' || s[n + len] == ')')) {
                if (!(bit & allowed)) {
                    return -EINVAL;
                }
                result |= bit;
                n += len + (s[n + len] == ',');
                break;
            }
        }

        if (!bit) {
            return -EINVAL;
        }
    }

    *res_flags = result;
    if (res_mask) {
        *res_mask = UINT32_MAX;
    }
    return n;
}

static void
format_odp_userspace_action(struct ds *ds, const struct nlattr *attr)
{
    static const struct nl_policy ovs_userspace_policy[] = {
        [OVS_USERSPACE_ATTR_PID] = { .type = NL_A_U32 },
        [OVS_USERSPACE_ATTR_USERDATA] = { .type = NL_A_UNSPEC,
                                          .optional = true },
        [OVS_USERSPACE_ATTR_EGRESS_TUN_PORT] = { .type = NL_A_U32,
                                                 .optional = true },
    };
    struct nlattr *a[ARRAY_SIZE(ovs_userspace_policy)];
    const struct nlattr *userdata_attr;
    const struct nlattr *tunnel_out_port_attr;

    if (!nl_parse_nested(attr, ovs_userspace_policy, a, ARRAY_SIZE(a))) {
        ds_put_cstr(ds, "userspace(error)");
        return;
    }

    ds_put_format(ds, "userspace(pid=%"PRIu32,
                  nl_attr_get_u32(a[OVS_USERSPACE_ATTR_PID]));

    userdata_attr = a[OVS_USERSPACE_ATTR_USERDATA];

    if (userdata_attr) {
        const uint8_t *userdata = nl_attr_get(userdata_attr);
        size_t userdata_len = nl_attr_get_size(userdata_attr);
        bool userdata_unspec = true;
        union user_action_cookie cookie;

        if (userdata_len >= sizeof cookie.type
            && userdata_len <= sizeof cookie) {

            memset(&cookie, 0, sizeof cookie);
            memcpy(&cookie, userdata, userdata_len);

            userdata_unspec = false;

            if (userdata_len == sizeof cookie.sflow
                && cookie.type == USER_ACTION_COOKIE_SFLOW) {
                ds_put_format(ds, ",sFlow("
                              "vid=%"PRIu16",pcp=%"PRIu8",output=%"PRIu32")",
                              vlan_tci_to_vid(cookie.sflow.vlan_tci),
                              vlan_tci_to_pcp(cookie.sflow.vlan_tci),
                              cookie.sflow.output);
            } else if (userdata_len == sizeof cookie.slow_path
                       && cookie.type == USER_ACTION_COOKIE_SLOW_PATH) {
                ds_put_cstr(ds, ",slow_path(");
                format_flags(ds, slow_path_reason_to_string,
                             cookie.slow_path.reason, ',');
                ds_put_format(ds, ")");
            } else if (userdata_len == sizeof cookie.flow_sample
                       && cookie.type == USER_ACTION_COOKIE_FLOW_SAMPLE) {
                ds_put_format(ds, ",flow_sample(probability=%"PRIu16
                              ",collector_set_id=%"PRIu32
                              ",obs_domain_id=%"PRIu32
                              ",obs_point_id=%"PRIu32")",
                              cookie.flow_sample.probability,
                              cookie.flow_sample.collector_set_id,
                              cookie.flow_sample.obs_domain_id,
                              cookie.flow_sample.obs_point_id);
            } else if (userdata_len >= sizeof cookie.ipfix
                       && cookie.type == USER_ACTION_COOKIE_IPFIX) {
                ds_put_format(ds, ",ipfix(output_port=%"PRIu32")",
                              cookie.ipfix.output_odp_port);
            } else {
                userdata_unspec = true;
            }
        }

        if (userdata_unspec) {
            size_t i;
            ds_put_format(ds, ",userdata(");
            for (i = 0; i < userdata_len; i++) {
                ds_put_format(ds, "%02x", userdata[i]);
            }
            ds_put_char(ds, ')');
        }
    }

    tunnel_out_port_attr = a[OVS_USERSPACE_ATTR_EGRESS_TUN_PORT];
    if (tunnel_out_port_attr) {
        ds_put_format(ds, ",tunnel_out_port=%"PRIu32,
                      nl_attr_get_u32(tunnel_out_port_attr));
    }

    ds_put_char(ds, ')');
}

static void
format_vlan_tci(struct ds *ds, ovs_be16 tci, ovs_be16 mask, bool verbose)
{
    if (verbose || vlan_tci_to_vid(tci) || vlan_tci_to_vid(mask)) {
        ds_put_format(ds, "vid=%"PRIu16, vlan_tci_to_vid(tci));
        if (vlan_tci_to_vid(mask) != VLAN_VID_MASK) { /* Partially masked. */
            ds_put_format(ds, "/0x%"PRIx16, vlan_tci_to_vid(mask));
        };
        ds_put_char(ds, ',');
    }
    if (verbose || vlan_tci_to_pcp(tci) || vlan_tci_to_pcp(mask)) {
        ds_put_format(ds, "pcp=%d", vlan_tci_to_pcp(tci));
        if (vlan_tci_to_pcp(mask) != (VLAN_PCP_MASK >> VLAN_PCP_SHIFT)) {
            ds_put_format(ds, "/0x%x", vlan_tci_to_pcp(mask));
        }
        ds_put_char(ds, ',');
    }
    if (!(tci & htons(VLAN_CFI))) {
        ds_put_cstr(ds, "cfi=0");
        ds_put_char(ds, ',');
    }
    ds_chomp(ds, ',');
}

static void
format_mpls_lse(struct ds *ds, ovs_be32 mpls_lse)
{
    ds_put_format(ds, "label=%"PRIu32",tc=%d,ttl=%d,bos=%d",
                  mpls_lse_to_label(mpls_lse),
                  mpls_lse_to_tc(mpls_lse),
                  mpls_lse_to_ttl(mpls_lse),
                  mpls_lse_to_bos(mpls_lse));
}

static void
format_mpls(struct ds *ds, const struct ovs_key_mpls *mpls_key,
            const struct ovs_key_mpls *mpls_mask, int n)
{
    if (n == 1) {
        ovs_be32 key = mpls_key->mpls_lse;

        if (mpls_mask == NULL) {
            format_mpls_lse(ds, key);
        } else {
            ovs_be32 mask = mpls_mask->mpls_lse;

            ds_put_format(ds, "label=%"PRIu32"/0x%x,tc=%d/%x,ttl=%d/0x%x,bos=%d/%x",
                          mpls_lse_to_label(key), mpls_lse_to_label(mask),
                          mpls_lse_to_tc(key), mpls_lse_to_tc(mask),
                          mpls_lse_to_ttl(key), mpls_lse_to_ttl(mask),
                          mpls_lse_to_bos(key), mpls_lse_to_bos(mask));
        }
    } else {
        int i;

        for (i = 0; i < n; i++) {
            ds_put_format(ds, "lse%d=%#"PRIx32,
                          i, ntohl(mpls_key[i].mpls_lse));
            if (mpls_mask) {
                ds_put_format(ds, "/%#"PRIx32, ntohl(mpls_mask[i].mpls_lse));
            }
            ds_put_char(ds, ',');
        }
        ds_chomp(ds, ',');
    }
}

static void
format_odp_recirc_action(struct ds *ds, uint32_t recirc_id)
{
    ds_put_format(ds, "recirc(%"PRIu32")", recirc_id);
}

static void
format_odp_hash_action(struct ds *ds, const struct ovs_action_hash *hash_act)
{
    ds_put_format(ds, "hash(");

    if (hash_act->hash_alg == OVS_HASH_ALG_L4) {
        ds_put_format(ds, "hash_l4(%"PRIu32")", hash_act->hash_basis);
    } else {
        ds_put_format(ds, "Unknown hash algorithm(%"PRIu32")",
                      hash_act->hash_alg);
    }
    ds_put_format(ds, ")");
}

static void
format_odp_action(struct ds *ds, const struct nlattr *a)
{
    int expected_len;
    enum ovs_action_attr type = nl_attr_type(a);
    const struct ovs_action_push_vlan *vlan;
    size_t size;

    expected_len = odp_action_len(nl_attr_type(a));
    if (expected_len != -2 && nl_attr_get_size(a) != expected_len) {
        ds_put_format(ds, "bad length %"PRIuSIZE", expected %d for: ",
                      nl_attr_get_size(a), expected_len);
        format_generic_odp_action(ds, a);
        return;
    }

    switch (type) {
    case OVS_ACTION_ATTR_OUTPUT:
        ds_put_format(ds, "%"PRIu32, nl_attr_get_u32(a));
        break;
    case OVS_ACTION_ATTR_USERSPACE:
        format_odp_userspace_action(ds, a);
        break;
    case OVS_ACTION_ATTR_RECIRC:
        format_odp_recirc_action(ds, nl_attr_get_u32(a));
        break;
    case OVS_ACTION_ATTR_HASH:
        format_odp_hash_action(ds, nl_attr_get(a));
        break;
    case OVS_ACTION_ATTR_SET_MASKED:
        a = nl_attr_get(a);
        size = nl_attr_get_size(a) / 2;
        ds_put_cstr(ds, "set(");

        /* Masked set action not supported for tunnel key, which is bigger. */
        if (size <= sizeof(struct ovs_key_ipv6)) {
            struct nlattr attr[1 + DIV_ROUND_UP(sizeof(struct ovs_key_ipv6),
                                                sizeof(struct nlattr))];
            struct nlattr mask[1 + DIV_ROUND_UP(sizeof(struct ovs_key_ipv6),
                                                sizeof(struct nlattr))];

            mask->nla_type = attr->nla_type = nl_attr_type(a);
            mask->nla_len = attr->nla_len = NLA_HDRLEN + size;
            memcpy(attr + 1, (char *)(a + 1), size);
            memcpy(mask + 1, (char *)(a + 1) + size, size);
            format_odp_key_attr(attr, mask, NULL, ds, false);
        } else {
            format_odp_key_attr(a, NULL, NULL, ds, false);
        }
        ds_put_cstr(ds, ")");
        break;
    case OVS_ACTION_ATTR_SET:
        ds_put_cstr(ds, "set(");
        format_odp_key_attr(nl_attr_get(a), NULL, NULL, ds, true);
        ds_put_cstr(ds, ")");
        break;
    case OVS_ACTION_ATTR_PUSH_VLAN:
        vlan = nl_attr_get(a);
        ds_put_cstr(ds, "push_vlan(");
        if (vlan->vlan_tpid != htons(ETH_TYPE_VLAN)) {
            ds_put_format(ds, "tpid=0x%04"PRIx16",", ntohs(vlan->vlan_tpid));
        }
        format_vlan_tci(ds, vlan->vlan_tci, OVS_BE16_MAX, false);
        ds_put_char(ds, ')');
        break;
    case OVS_ACTION_ATTR_POP_VLAN:
        ds_put_cstr(ds, "pop_vlan");
        break;
    case OVS_ACTION_ATTR_PUSH_MPLS: {
        const struct ovs_action_push_mpls *mpls = nl_attr_get(a);
        ds_put_cstr(ds, "push_mpls(");
        format_mpls_lse(ds, mpls->mpls_lse);
        ds_put_format(ds, ",eth_type=0x%"PRIx16")", ntohs(mpls->mpls_ethertype));
        break;
    }
    case OVS_ACTION_ATTR_POP_MPLS: {
        ovs_be16 ethertype = nl_attr_get_be16(a);
        ds_put_format(ds, "pop_mpls(eth_type=0x%"PRIx16")", ntohs(ethertype));
        break;
    }
    case OVS_ACTION_ATTR_SAMPLE:
        format_odp_sample_action(ds, a);
        break;
    case OVS_ACTION_ATTR_UNSPEC:
    case __OVS_ACTION_ATTR_MAX:
    default:
        format_generic_odp_action(ds, a);
        break;
    }
}

void
format_odp_actions(struct ds *ds, const struct nlattr *actions,
                   size_t actions_len)
{
    if (actions_len) {
        const struct nlattr *a;
        unsigned int left;

        NL_ATTR_FOR_EACH (a, left, actions, actions_len) {
            if (a != actions) {
                ds_put_char(ds, ',');
            }
            format_odp_action(ds, a);
        }
        if (left) {
            int i;

            if (left == actions_len) {
                ds_put_cstr(ds, "<empty>");
            }
            ds_put_format(ds, ",***%u leftover bytes*** (", left);
            for (i = 0; i < left; i++) {
                ds_put_format(ds, "%02x", ((const uint8_t *) a)[i]);
            }
            ds_put_char(ds, ')');
        }
    } else {
        ds_put_cstr(ds, "drop");
    }
}

/* Separate out parse_odp_userspace_action() function. */
static int
parse_odp_userspace_action(const char *s, struct ofpbuf *actions)
{
    uint32_t pid;
    union user_action_cookie cookie;
    struct ofpbuf buf;
    odp_port_t tunnel_out_port;
    int n = -1;
    void *user_data = NULL;
    size_t user_data_size = 0;

    if (!ovs_scan(s, "userspace(pid=%"SCNi32"%n", &pid, &n)) {
        return -EINVAL;
    }

    {
        uint32_t output;
        uint32_t probability;
        uint32_t collector_set_id;
        uint32_t obs_domain_id;
        uint32_t obs_point_id;
        int vid, pcp;
        int n1 = -1;
        if (ovs_scan(&s[n], ",sFlow(vid=%i,"
                     "pcp=%i,output=%"SCNi32")%n",
                     &vid, &pcp, &output, &n1)) {
            uint16_t tci;

            n += n1;
            tci = vid | (pcp << VLAN_PCP_SHIFT);
            if (tci) {
                tci |= VLAN_CFI;
            }

            cookie.type = USER_ACTION_COOKIE_SFLOW;
            cookie.sflow.vlan_tci = htons(tci);
            cookie.sflow.output = output;
            user_data = &cookie;
            user_data_size = sizeof cookie.sflow;
        } else if (ovs_scan(&s[n], ",slow_path(%n",
                            &n1)) {
            int res;

            n += n1;
            cookie.type = USER_ACTION_COOKIE_SLOW_PATH;
            cookie.slow_path.unused = 0;
            cookie.slow_path.reason = 0;

            res = parse_flags(&s[n], slow_path_reason_to_string,
                              &cookie.slow_path.reason,
                              SLOW_PATH_REASON_MASK, NULL);
            if (res < 0 || s[n + res] != ')') {
                return res;
            }
            n += res + 1;

            user_data = &cookie;
            user_data_size = sizeof cookie.slow_path;
        } else if (ovs_scan(&s[n], ",flow_sample(probability=%"SCNi32","
                            "collector_set_id=%"SCNi32","
                            "obs_domain_id=%"SCNi32","
                            "obs_point_id=%"SCNi32")%n",
                            &probability, &collector_set_id,
                            &obs_domain_id, &obs_point_id, &n1)) {
            n += n1;

            cookie.type = USER_ACTION_COOKIE_FLOW_SAMPLE;
            cookie.flow_sample.probability = probability;
            cookie.flow_sample.collector_set_id = collector_set_id;
            cookie.flow_sample.obs_domain_id = obs_domain_id;
            cookie.flow_sample.obs_point_id = obs_point_id;
            user_data = &cookie;
            user_data_size = sizeof cookie.flow_sample;
        } else if (ovs_scan(&s[n], ",ipfix(output_port=%"SCNi32")%n",
                            &output, &n1) ) {
            n += n1;
            cookie.type = USER_ACTION_COOKIE_IPFIX;
            cookie.ipfix.output_odp_port = u32_to_odp(output);
            user_data = &cookie;
            user_data_size = sizeof cookie.ipfix;
        } else if (ovs_scan(&s[n], ",userdata(%n",
                            &n1)) {
            char *end;

            n += n1;
            ofpbuf_init(&buf, 16);
            end = ofpbuf_put_hex(&buf, &s[n], NULL);
            if (end[0] != ')') {
                return -EINVAL;
            }
            user_data = ofpbuf_data(&buf);
            user_data_size = ofpbuf_size(&buf);
            n = (end + 1) - s;
        }
    }

    {
        int n1 = -1;
        if (ovs_scan(&s[n], ",tunnel_out_port=%"SCNi32")%n",
                     &tunnel_out_port, &n1)) {
            odp_put_userspace_action(pid, user_data, user_data_size, tunnel_out_port, actions);
            return n + n1;
        } else if (s[n] == ')') {
            odp_put_userspace_action(pid, user_data, user_data_size, ODPP_NONE, actions);
            return n + 1;
        }
    }

    return -EINVAL;
}

static int
parse_odp_action(const char *s, const struct simap *port_names,
                 struct ofpbuf *actions)
{
    {
        uint32_t port;
        int n;

        if (ovs_scan(s, "%"SCNi32"%n", &port, &n)) {
            nl_msg_put_u32(actions, OVS_ACTION_ATTR_OUTPUT, port);
            return n;
        }
    }

    if (port_names) {
        int len = strcspn(s, delimiters);
        struct simap_node *node;

        node = simap_find_len(port_names, s, len);
        if (node) {
            nl_msg_put_u32(actions, OVS_ACTION_ATTR_OUTPUT, node->data);
            return len;
        }
    }

    if (!strncmp(s, "userspace(", 10)) {
        return parse_odp_userspace_action(s, actions);
    }

    if (!strncmp(s, "set(", 4)) {
        size_t start_ofs;
        int retval;
        struct nlattr mask[128 / sizeof(struct nlattr)];
        struct ofpbuf maskbuf;
        struct nlattr *nested, *key;
        size_t size;

        /* 'mask' is big enough to hold any key. */
        ofpbuf_use_stack(&maskbuf, mask, sizeof mask);

        start_ofs = nl_msg_start_nested(actions, OVS_ACTION_ATTR_SET);
        retval = parse_odp_key_mask_attr(s + 4, port_names, actions, &maskbuf);
        if (retval < 0) {
            return retval;
        }
        if (s[retval + 4] != ')') {
            return -EINVAL;
        }

        nested = ofpbuf_at_assert(actions, start_ofs, sizeof *nested);
        key = nested + 1;

        size = nl_attr_get_size(mask);
        if (size == nl_attr_get_size(key)) {
            /* Change to masked set action if not fully masked. */
            if (!is_all_ones(mask + 1, size)) {
                key->nla_len += size;
                ofpbuf_put(actions, mask + 1, size);
                /* 'actions' may have been reallocated by ofpbuf_put(). */
                nested = ofpbuf_at_assert(actions, start_ofs, sizeof *nested);
                nested->nla_type = OVS_ACTION_ATTR_SET_MASKED;
            }
        }

        nl_msg_end_nested(actions, start_ofs);
        return retval + 5;
    }

    {
        struct ovs_action_push_vlan push;
        int tpid = ETH_TYPE_VLAN;
        int vid, pcp;
        int cfi = 1;
        int n = -1;

        if (ovs_scan(s, "push_vlan(vid=%i,pcp=%i)%n", &vid, &pcp, &n)
            || ovs_scan(s, "push_vlan(vid=%i,pcp=%i,cfi=%i)%n",
                        &vid, &pcp, &cfi, &n)
            || ovs_scan(s, "push_vlan(tpid=%i,vid=%i,pcp=%i)%n",
                        &tpid, &vid, &pcp, &n)
            || ovs_scan(s, "push_vlan(tpid=%i,vid=%i,pcp=%i,cfi=%i)%n",
                        &tpid, &vid, &pcp, &cfi, &n)) {
            push.vlan_tpid = htons(tpid);
            push.vlan_tci = htons((vid << VLAN_VID_SHIFT)
                                  | (pcp << VLAN_PCP_SHIFT)
                                  | (cfi ? VLAN_CFI : 0));
            nl_msg_put_unspec(actions, OVS_ACTION_ATTR_PUSH_VLAN,
                              &push, sizeof push);

            return n;
        }
    }

    if (!strncmp(s, "pop_vlan", 8)) {
        nl_msg_put_flag(actions, OVS_ACTION_ATTR_POP_VLAN);
        return 8;
    }

    {
        double percentage;
        int n = -1;

        if (ovs_scan(s, "sample(sample=%lf%%,actions(%n", &percentage, &n)
            && percentage >= 0. && percentage <= 100.0) {
            size_t sample_ofs, actions_ofs;
            double probability;

            probability = floor(UINT32_MAX * (percentage / 100.0) + .5);
            sample_ofs = nl_msg_start_nested(actions, OVS_ACTION_ATTR_SAMPLE);
            nl_msg_put_u32(actions, OVS_SAMPLE_ATTR_PROBABILITY,
                           (probability <= 0 ? 0
                            : probability >= UINT32_MAX ? UINT32_MAX
                            : probability));

            actions_ofs = nl_msg_start_nested(actions,
                                              OVS_SAMPLE_ATTR_ACTIONS);
            for (;;) {
                int retval;

                n += strspn(s + n, delimiters);
                if (s[n] == ')') {
                    break;
                }

                retval = parse_odp_action(s + n, port_names, actions);
                if (retval < 0) {
                    return retval;
                }
                n += retval;
            }
            nl_msg_end_nested(actions, actions_ofs);
            nl_msg_end_nested(actions, sample_ofs);

            return s[n + 1] == ')' ? n + 2 : -EINVAL;
        }
    }

    return -EINVAL;
}

/* Parses the string representation of datapath actions, in the format output
 * by format_odp_action().  Returns 0 if successful, otherwise a positive errno
 * value.  On success, the ODP actions are appended to 'actions' as a series of
 * Netlink attributes.  On failure, no data is appended to 'actions'.  Either
 * way, 'actions''s data might be reallocated. */
int
odp_actions_from_string(const char *s, const struct simap *port_names,
                        struct ofpbuf *actions)
{
    size_t old_size;

    if (!strcasecmp(s, "drop")) {
        return 0;
    }

    old_size = ofpbuf_size(actions);
    for (;;) {
        int retval;

        s += strspn(s, delimiters);
        if (!*s) {
            return 0;
        }

        retval = parse_odp_action(s, port_names, actions);
        if (retval < 0 || !strchr(delimiters, s[retval])) {
            ofpbuf_set_size(actions, old_size);
            return -retval;
        }
        s += retval;
    }

    return 0;
}

/* Returns the correct length of the payload for a flow key attribute of the
 * specified 'type', -1 if 'type' is unknown, or -2 if the attribute's payload
 * is variable length. */
static int
odp_flow_key_attr_len(uint16_t type)
{
    if (type > OVS_KEY_ATTR_MAX) {
        return -1;
    }

    switch ((enum ovs_key_attr) type) {
    case OVS_KEY_ATTR_ENCAP: return -2;
    case OVS_KEY_ATTR_PRIORITY: return 4;
    case OVS_KEY_ATTR_SKB_MARK: return 4;
    case OVS_KEY_ATTR_DP_HASH: return 4;
    case OVS_KEY_ATTR_RECIRC_ID: return 4;
    case OVS_KEY_ATTR_TUNNEL: return -2;
    case OVS_KEY_ATTR_IN_PORT: return 4;
    case OVS_KEY_ATTR_ETHERNET: return sizeof(struct ovs_key_ethernet);
    case OVS_KEY_ATTR_VLAN: return sizeof(ovs_be16);
    case OVS_KEY_ATTR_ETHERTYPE: return 2;
    case OVS_KEY_ATTR_MPLS: return -2;
    case OVS_KEY_ATTR_IPV4: return sizeof(struct ovs_key_ipv4);
    case OVS_KEY_ATTR_IPV6: return sizeof(struct ovs_key_ipv6);
    case OVS_KEY_ATTR_TCP: return sizeof(struct ovs_key_tcp);
    case OVS_KEY_ATTR_TCP_FLAGS: return 2;
    case OVS_KEY_ATTR_UDP: return sizeof(struct ovs_key_udp);
    case OVS_KEY_ATTR_SCTP: return sizeof(struct ovs_key_sctp);
    case OVS_KEY_ATTR_ICMP: return sizeof(struct ovs_key_icmp);
    case OVS_KEY_ATTR_ICMPV6: return sizeof(struct ovs_key_icmpv6);
    case OVS_KEY_ATTR_ARP: return sizeof(struct ovs_key_arp);
    case OVS_KEY_ATTR_ND: return sizeof(struct ovs_key_nd);

    case OVS_KEY_ATTR_UNSPEC:
    case __OVS_KEY_ATTR_MAX:
        return -1;
    }

    return -1;
}

static void
format_generic_odp_key(const struct nlattr *a, struct ds *ds)
{
    size_t len = nl_attr_get_size(a);
    if (len) {
        const uint8_t *unspec;
        unsigned int i;

        unspec = nl_attr_get(a);
        for (i = 0; i < len; i++) {
            if (i) {
                ds_put_char(ds, ' ');
            }
            ds_put_format(ds, "%02x", unspec[i]);
        }
    }
}

static const char *
ovs_frag_type_to_string(enum ovs_frag_type type)
{
    switch (type) {
    case OVS_FRAG_TYPE_NONE:
        return "no";
    case OVS_FRAG_TYPE_FIRST:
        return "first";
    case OVS_FRAG_TYPE_LATER:
        return "later";
    case __OVS_FRAG_TYPE_MAX:
    default:
        return "<error>";
    }
}

static int
tunnel_key_attr_len(int type)
{
    switch (type) {
    case OVS_TUNNEL_KEY_ATTR_ID: return 8;
    case OVS_TUNNEL_KEY_ATTR_IPV4_SRC: return 4;
    case OVS_TUNNEL_KEY_ATTR_IPV4_DST: return 4;
    case OVS_TUNNEL_KEY_ATTR_TOS: return 1;
    case OVS_TUNNEL_KEY_ATTR_TTL: return 1;
    case OVS_TUNNEL_KEY_ATTR_DONT_FRAGMENT: return 0;
    case OVS_TUNNEL_KEY_ATTR_CSUM: return 0;
    case OVS_TUNNEL_KEY_ATTR_TP_SRC: return 2;
    case OVS_TUNNEL_KEY_ATTR_TP_DST: return 2;
    case OVS_TUNNEL_KEY_ATTR_OAM: return 0;
    case OVS_TUNNEL_KEY_ATTR_GENEVE_OPTS: return -2;
    case __OVS_TUNNEL_KEY_ATTR_MAX:
        return -1;
    }
    return -1;
}

#define GENEVE_OPT(class, type) ((OVS_FORCE uint32_t)(class) << 8 | (type))
static int
parse_geneve_opts(const struct nlattr *attr)
{
    int opts_len = nl_attr_get_size(attr);
    const struct geneve_opt *opt = nl_attr_get(attr);

    while (opts_len > 0) {
        int len;

        if (opts_len < sizeof(*opt)) {
            return -EINVAL;
        }

        len = sizeof(*opt) + opt->length * 4;
        if (len > opts_len) {
            return -EINVAL;
        }

        switch (GENEVE_OPT(opt->opt_class, opt->type)) {
        default:
            if (opt->type & GENEVE_CRIT_OPT_TYPE) {
                return -EINVAL;
            }
        };

        opt = opt + len / sizeof(*opt);
        opts_len -= len;
    };

    return 0;
}

enum odp_key_fitness
odp_tun_key_from_attr(const struct nlattr *attr, struct flow_tnl *tun)
{
    unsigned int left;
    const struct nlattr *a;
    bool ttl = false;
    bool unknown = false;

    NL_NESTED_FOR_EACH(a, left, attr) {
        uint16_t type = nl_attr_type(a);
        size_t len = nl_attr_get_size(a);
        int expected_len = tunnel_key_attr_len(type);

        if (len != expected_len && expected_len >= 0) {
            return ODP_FIT_ERROR;
        }

        switch (type) {
        case OVS_TUNNEL_KEY_ATTR_ID:
            tun->tun_id = nl_attr_get_be64(a);
            tun->flags |= FLOW_TNL_F_KEY;
            break;
        case OVS_TUNNEL_KEY_ATTR_IPV4_SRC:
            tun->ip_src = nl_attr_get_be32(a);
            break;
        case OVS_TUNNEL_KEY_ATTR_IPV4_DST:
            tun->ip_dst = nl_attr_get_be32(a);
            break;
        case OVS_TUNNEL_KEY_ATTR_TOS:
            tun->ip_tos = nl_attr_get_u8(a);
            break;
        case OVS_TUNNEL_KEY_ATTR_TTL:
            tun->ip_ttl = nl_attr_get_u8(a);
            ttl = true;
            break;
        case OVS_TUNNEL_KEY_ATTR_DONT_FRAGMENT:
            tun->flags |= FLOW_TNL_F_DONT_FRAGMENT;
            break;
        case OVS_TUNNEL_KEY_ATTR_CSUM:
            tun->flags |= FLOW_TNL_F_CSUM;
            break;
        case OVS_TUNNEL_KEY_ATTR_TP_SRC:
            tun->tp_src = nl_attr_get_be16(a);
            break;
        case OVS_TUNNEL_KEY_ATTR_TP_DST:
            tun->tp_dst = nl_attr_get_be16(a);
            break;
        case OVS_TUNNEL_KEY_ATTR_OAM:
            tun->flags |= FLOW_TNL_F_OAM;
            break;
        case OVS_TUNNEL_KEY_ATTR_GENEVE_OPTS: {
            if (parse_geneve_opts(a)) {
                return ODP_FIT_ERROR;
            }
            /* It is necessary to reproduce options exactly (including order)
             * so it's easiest to just echo them back. */
            unknown = true;
            break;
        }
        default:
            /* Allow this to show up as unexpected, if there are unknown
             * tunnel attribute, eventually resulting in ODP_FIT_TOO_MUCH. */
            unknown = true;
            break;
        }
    }

    if (!ttl) {
        return ODP_FIT_ERROR;
    }
    if (unknown) {
        return ODP_FIT_TOO_MUCH;
    }
    return ODP_FIT_PERFECT;
}

static void
tun_key_to_attr(struct ofpbuf *a, const struct flow_tnl *tun_key)
{
    size_t tun_key_ofs;

    tun_key_ofs = nl_msg_start_nested(a, OVS_KEY_ATTR_TUNNEL);

    /* tun_id != 0 without FLOW_TNL_F_KEY is valid if tun_key is a mask. */
    if (tun_key->tun_id || tun_key->flags & FLOW_TNL_F_KEY) {
        nl_msg_put_be64(a, OVS_TUNNEL_KEY_ATTR_ID, tun_key->tun_id);
    }
    if (tun_key->ip_src) {
        nl_msg_put_be32(a, OVS_TUNNEL_KEY_ATTR_IPV4_SRC, tun_key->ip_src);
    }
    if (tun_key->ip_dst) {
        nl_msg_put_be32(a, OVS_TUNNEL_KEY_ATTR_IPV4_DST, tun_key->ip_dst);
    }
    if (tun_key->ip_tos) {
        nl_msg_put_u8(a, OVS_TUNNEL_KEY_ATTR_TOS, tun_key->ip_tos);
    }
    nl_msg_put_u8(a, OVS_TUNNEL_KEY_ATTR_TTL, tun_key->ip_ttl);
    if (tun_key->flags & FLOW_TNL_F_DONT_FRAGMENT) {
        nl_msg_put_flag(a, OVS_TUNNEL_KEY_ATTR_DONT_FRAGMENT);
    }
    if (tun_key->flags & FLOW_TNL_F_CSUM) {
        nl_msg_put_flag(a, OVS_TUNNEL_KEY_ATTR_CSUM);
    }
    if (tun_key->tp_src) {
        nl_msg_put_be16(a, OVS_TUNNEL_KEY_ATTR_TP_SRC, tun_key->tp_src);
    }
    if (tun_key->tp_dst) {
        nl_msg_put_be16(a, OVS_TUNNEL_KEY_ATTR_TP_DST, tun_key->tp_dst);
    }
    if (tun_key->flags & FLOW_TNL_F_OAM) {
        nl_msg_put_flag(a, OVS_TUNNEL_KEY_ATTR_OAM);
    }

    nl_msg_end_nested(a, tun_key_ofs);
}

static bool
odp_mask_attr_is_wildcard(const struct nlattr *ma)
{
    return is_all_zeros(nl_attr_get(ma), nl_attr_get_size(ma));
}

static bool
odp_mask_attr_is_exact(const struct nlattr *ma)
{
    bool is_exact;
    enum ovs_key_attr attr = nl_attr_type(ma);

    if (attr == OVS_KEY_ATTR_TCP_FLAGS) {
        is_exact = TCP_FLAGS(nl_attr_get_be16(ma)) == TCP_FLAGS(OVS_BE16_MAX);
    } else if (attr == OVS_KEY_ATTR_IPV6) {
        const struct ovs_key_ipv6 *mask = nl_attr_get(ma);

        is_exact =
            ((mask->ipv6_label & htonl(IPV6_LABEL_MASK))
             == htonl(IPV6_LABEL_MASK))
            && mask->ipv6_proto == UINT8_MAX
            && mask->ipv6_tclass == UINT8_MAX
            && mask->ipv6_hlimit == UINT8_MAX
            && mask->ipv6_frag == UINT8_MAX
            && ipv6_mask_is_exact((const struct in6_addr *)mask->ipv6_src)
            && ipv6_mask_is_exact((const struct in6_addr *)mask->ipv6_dst);
    } else if (attr == OVS_KEY_ATTR_TUNNEL) {
        struct flow_tnl tun_mask;

        memset(&tun_mask, 0, sizeof tun_mask);
        odp_tun_key_from_attr(ma, &tun_mask);
        is_exact = tun_mask.flags == FLOW_TNL_F_MASK
            && tun_mask.tun_id == OVS_BE64_MAX
            && tun_mask.ip_src == OVS_BE32_MAX
            && tun_mask.ip_dst == OVS_BE32_MAX
            && tun_mask.ip_tos == UINT8_MAX
            && tun_mask.ip_ttl == UINT8_MAX
            && tun_mask.tp_src == OVS_BE16_MAX
            && tun_mask.tp_dst == OVS_BE16_MAX;
    } else {
        is_exact = is_all_ones(nl_attr_get(ma), nl_attr_get_size(ma));
    }

    return is_exact;
}

void
odp_portno_names_set(struct hmap *portno_names, odp_port_t port_no,
                     char *port_name)
{
    struct odp_portno_names *odp_portno_names;

    odp_portno_names = xmalloc(sizeof *odp_portno_names);
    odp_portno_names->port_no = port_no;
    odp_portno_names->name = xstrdup(port_name);
    hmap_insert(portno_names, &odp_portno_names->hmap_node,
                hash_odp_port(port_no));
}

static char *
odp_portno_names_get(const struct hmap *portno_names, odp_port_t port_no)
{
    struct odp_portno_names *odp_portno_names;

    HMAP_FOR_EACH_IN_BUCKET (odp_portno_names, hmap_node,
                             hash_odp_port(port_no), portno_names) {
        if (odp_portno_names->port_no == port_no) {
            return odp_portno_names->name;
        }
    }
    return NULL;
}

void
odp_portno_names_destroy(struct hmap *portno_names)
{
    struct odp_portno_names *odp_portno_names, *odp_portno_names_next;
    HMAP_FOR_EACH_SAFE (odp_portno_names, odp_portno_names_next,
                        hmap_node, portno_names) {
        hmap_remove(portno_names, &odp_portno_names->hmap_node);
        free(odp_portno_names->name);
        free(odp_portno_names);
    }
}

/* Format helpers. */

static void
format_eth(struct ds *ds, const char *name, const uint8_t key[ETH_ADDR_LEN],
           const uint8_t (*mask)[ETH_ADDR_LEN], bool verbose)
{
    bool mask_empty = mask && eth_addr_is_zero(*mask);

    if (verbose || !mask_empty) {
        bool mask_full = !mask || eth_mask_is_exact(*mask);

        if (mask_full) {
            ds_put_format(ds, "%s="ETH_ADDR_FMT",", name, ETH_ADDR_ARGS(key));
        } else {
            ds_put_format(ds, "%s=", name);
            eth_format_masked(key, *mask, ds);
            ds_put_char(ds, ',');
        }
    }
}

static void
format_be64(struct ds *ds, const char *name, ovs_be64 key,
            const ovs_be64 *mask, bool verbose)
{
    bool mask_empty = mask && !*mask;

    if (verbose || !mask_empty) {
        bool mask_full = !mask || *mask == OVS_BE64_MAX;

        ds_put_format(ds, "%s=0x%"PRIx64, name, ntohll(key));
        if (!mask_full) { /* Partially masked. */
            ds_put_format(ds, "/%#"PRIx64, ntohll(*mask));
        }
        ds_put_char(ds, ',');
    }
}

static void
format_ipv4(struct ds *ds, const char *name, ovs_be32 key,
            const ovs_be32 *mask, bool verbose)
{
    bool mask_empty = mask && !*mask;

    if (verbose || !mask_empty) {
        bool mask_full = !mask || *mask == OVS_BE32_MAX;

        ds_put_format(ds, "%s="IP_FMT, name, IP_ARGS(key));
        if (!mask_full) { /* Partially masked. */
            ds_put_format(ds, "/"IP_FMT, IP_ARGS(*mask));
        }
        ds_put_char(ds, ',');
    }
}

static void
format_ipv6(struct ds *ds, const char *name, const ovs_be32 key_[4],
            const ovs_be32 (*mask_)[4], bool verbose)
{
    char buf[INET6_ADDRSTRLEN];
    const struct in6_addr *key = (const struct in6_addr *)key_;
    const struct in6_addr *mask = mask_ ? (const struct in6_addr *)*mask_
        : NULL;
    bool mask_empty = mask && ipv6_mask_is_any(mask);

    if (verbose || !mask_empty) {
        bool mask_full = !mask || ipv6_mask_is_exact(mask);

        inet_ntop(AF_INET6, key, buf, sizeof buf);
        ds_put_format(ds, "%s=%s", name, buf);
        if (!mask_full) { /* Partially masked. */
            inet_ntop(AF_INET6, mask, buf, sizeof buf);
            ds_put_format(ds, "/%s", buf);
        }
        ds_put_char(ds, ',');
    }
}

static void
format_ipv6_label(struct ds *ds, const char *name, ovs_be32 key,
                  const ovs_be32 *mask, bool verbose)
{
    bool mask_empty = mask && !*mask;

    if (verbose || !mask_empty) {
        bool mask_full = !mask
            || (*mask & htonl(IPV6_LABEL_MASK)) == htonl(IPV6_LABEL_MASK);

        ds_put_format(ds, "%s=%#"PRIx32, name, ntohl(key));
        if (!mask_full) { /* Partially masked. */
            ds_put_format(ds, "/%#"PRIx32, ntohl(*mask));
        }
        ds_put_char(ds, ',');
    }
}

static void
format_u8x(struct ds *ds, const char *name, uint8_t key,
           const uint8_t *mask, bool verbose)
{
    bool mask_empty = mask && !*mask;

    if (verbose || !mask_empty) {
        bool mask_full = !mask || *mask == UINT8_MAX;

        ds_put_format(ds, "%s=%#"PRIx8, name, key);
        if (!mask_full) { /* Partially masked. */
            ds_put_format(ds, "/%#"PRIx8, *mask);
        }
        ds_put_char(ds, ',');
    }
}

static void
format_u8u(struct ds *ds, const char *name, uint8_t key,
           const uint8_t *mask, bool verbose)
{
    bool mask_empty = mask && !*mask;

    if (verbose || !mask_empty) {
        bool mask_full = !mask || *mask == UINT8_MAX;

        ds_put_format(ds, "%s=%"PRIu8, name, key);
        if (!mask_full) { /* Partially masked. */
            ds_put_format(ds, "/%#"PRIx8, *mask);
        }
        ds_put_char(ds, ',');
    }
}

static void
format_be16(struct ds *ds, const char *name, ovs_be16 key,
            const ovs_be16 *mask, bool verbose)
{
    bool mask_empty = mask && !*mask;

    if (verbose || !mask_empty) {
        bool mask_full = !mask || *mask == OVS_BE16_MAX;

        ds_put_format(ds, "%s=%"PRIu16, name, ntohs(key));
        if (!mask_full) { /* Partially masked. */
            ds_put_format(ds, "/%#"PRIx16, ntohs(*mask));
        }
        ds_put_char(ds, ',');
    }
}

static void
format_tun_flags(struct ds *ds, const char *name, uint16_t key,
                 const uint16_t *mask, bool verbose)
{
    bool mask_empty = mask && !*mask;

    if (verbose || !mask_empty) {
        bool mask_full = !mask || (*mask & FLOW_TNL_F_MASK) == FLOW_TNL_F_MASK;

        ds_put_cstr(ds, name);
        ds_put_char(ds, '(');
        if (!mask_full) { /* Partially masked. */
            format_flags_masked(ds, NULL, flow_tun_flag_to_string, key, *mask);
        } else { /* Fully masked. */
            format_flags(ds, flow_tun_flag_to_string, key, ',');
        }
        ds_put_cstr(ds, "),");
    }
}

static void
format_frag(struct ds *ds, const char *name, uint8_t key,
            const uint8_t *mask, bool verbose)
{
    bool mask_empty = mask && !*mask;

    /* ODP frag is an enumeration field; partial masks are not meaningful. */
    if (verbose || !mask_empty) {
        bool mask_full = !mask || *mask == UINT8_MAX;

        if (!mask_full) { /* Partially masked. */
            ds_put_format(ds, "error: partial mask not supported for frag (%#"
                          PRIx8"),", *mask);
        } else {
            ds_put_format(ds, "%s=%s,", name, ovs_frag_type_to_string(key));
        }
    }
}

#define MASK(PTR, FIELD) PTR ? &PTR->FIELD : NULL

static void
format_odp_key_attr(const struct nlattr *a, const struct nlattr *ma,
                    const struct hmap *portno_names, struct ds *ds,
                    bool verbose)
{
    enum ovs_key_attr attr = nl_attr_type(a);
    char namebuf[OVS_KEY_ATTR_BUFSIZE];
    int expected_len;
    bool is_exact;

    is_exact = ma ? odp_mask_attr_is_exact(ma) : true;

    ds_put_cstr(ds, ovs_key_attr_to_string(attr, namebuf, sizeof namebuf));

    {
        expected_len = odp_flow_key_attr_len(nl_attr_type(a));
        if (expected_len != -2) {
            bool bad_key_len = nl_attr_get_size(a) != expected_len;
            bool bad_mask_len = ma && nl_attr_get_size(ma) != expected_len;

            if (bad_key_len || bad_mask_len) {
                if (bad_key_len) {
                    ds_put_format(ds, "(bad key length %"PRIuSIZE", expected %d)(",
                                  nl_attr_get_size(a), expected_len);
                }
                format_generic_odp_key(a, ds);
                if (ma) {
                    ds_put_char(ds, '/');
                    if (bad_mask_len) {
                        ds_put_format(ds, "(bad mask length %"PRIuSIZE", expected %d)(",
                                      nl_attr_get_size(ma), expected_len);
                    }
                    format_generic_odp_key(ma, ds);
                }
                ds_put_char(ds, ')');
                return;
            }
        }
    }

    ds_put_char(ds, '(');
    switch (attr) {
    case OVS_KEY_ATTR_ENCAP:
        if (ma && nl_attr_get_size(ma) && nl_attr_get_size(a)) {
            odp_flow_format(nl_attr_get(a), nl_attr_get_size(a),
                            nl_attr_get(ma), nl_attr_get_size(ma), NULL, ds,
                            verbose);
        } else if (nl_attr_get_size(a)) {
            odp_flow_format(nl_attr_get(a), nl_attr_get_size(a), NULL, 0, NULL,
                            ds, verbose);
        }
        break;

    case OVS_KEY_ATTR_PRIORITY:
    case OVS_KEY_ATTR_SKB_MARK:
    case OVS_KEY_ATTR_DP_HASH:
    case OVS_KEY_ATTR_RECIRC_ID:
        ds_put_format(ds, "%#"PRIx32, nl_attr_get_u32(a));
        if (!is_exact) {
            ds_put_format(ds, "/%#"PRIx32, nl_attr_get_u32(ma));
        }
        break;

    case OVS_KEY_ATTR_TUNNEL: {
        struct flow_tnl key, mask_;
        struct flow_tnl *mask = ma ? &mask_ : NULL;

        if (mask) {
            memset(mask, 0, sizeof *mask);
            odp_tun_key_from_attr(ma, mask);
        }
        memset(&key, 0, sizeof key);
        if (odp_tun_key_from_attr(a, &key) == ODP_FIT_ERROR) {
            ds_put_format(ds, "error");
            return;
        }
        format_be64(ds, "tun_id", key.tun_id, MASK(mask, tun_id), verbose);
        format_ipv4(ds, "src", key.ip_src, MASK(mask, ip_src), verbose);
        format_ipv4(ds, "dst", key.ip_dst, MASK(mask, ip_dst), verbose);
        format_u8x(ds, "tos", key.ip_tos, MASK(mask, ip_tos), verbose);
        format_u8u(ds, "ttl", key.ip_ttl, MASK(mask, ip_ttl), verbose);
        format_be16(ds, "tp_src", key.tp_src, MASK(mask, tp_src), verbose);
        format_be16(ds, "tp_dst", key.tp_dst, MASK(mask, tp_dst), verbose);
        format_tun_flags(ds, "flags", key.flags, MASK(mask, flags), verbose);
        ds_chomp(ds, ',');
        break;
    }
    case OVS_KEY_ATTR_IN_PORT:
        if (portno_names && verbose && is_exact) {
            char *name = odp_portno_names_get(portno_names,
                            u32_to_odp(nl_attr_get_u32(a)));
            if (name) {
                ds_put_format(ds, "%s", name);
            } else {
                ds_put_format(ds, "%"PRIu32, nl_attr_get_u32(a));
            }
        } else {
            ds_put_format(ds, "%"PRIu32, nl_attr_get_u32(a));
            if (!is_exact) {
                ds_put_format(ds, "/%#"PRIx32, nl_attr_get_u32(ma));
            }
        }
        break;

    case OVS_KEY_ATTR_ETHERNET: {
        const struct ovs_key_ethernet *mask = ma ? nl_attr_get(ma) : NULL;
        const struct ovs_key_ethernet *key = nl_attr_get(a);

        format_eth(ds, "src", key->eth_src, MASK(mask, eth_src), verbose);
        format_eth(ds, "dst", key->eth_dst, MASK(mask, eth_dst), verbose);
        ds_chomp(ds, ',');
        break;
    }
    case OVS_KEY_ATTR_VLAN:
        format_vlan_tci(ds, nl_attr_get_be16(a),
                        ma ? nl_attr_get_be16(ma) : OVS_BE16_MAX, verbose);
        break;

    case OVS_KEY_ATTR_MPLS: {
        const struct ovs_key_mpls *mpls_key = nl_attr_get(a);
        const struct ovs_key_mpls *mpls_mask = NULL;
        size_t size = nl_attr_get_size(a);

        if (!size || size % sizeof *mpls_key) {
            ds_put_format(ds, "(bad key length %"PRIuSIZE")", size);
            return;
        }
        if (!is_exact) {
            mpls_mask = nl_attr_get(ma);
            if (size != nl_attr_get_size(ma)) {
                ds_put_format(ds, "(key length %"PRIuSIZE" != "
                              "mask length %"PRIuSIZE")",
                              size, nl_attr_get_size(ma));
                return;
            }
        }
        format_mpls(ds, mpls_key, mpls_mask, size / sizeof *mpls_key);
        break;
    }
    case OVS_KEY_ATTR_ETHERTYPE:
        ds_put_format(ds, "0x%04"PRIx16, ntohs(nl_attr_get_be16(a)));
        if (!is_exact) {
            ds_put_format(ds, "/0x%04"PRIx16, ntohs(nl_attr_get_be16(ma)));
        }
        break;

    case OVS_KEY_ATTR_IPV4: {
        const struct ovs_key_ipv4 *key = nl_attr_get(a);
        const struct ovs_key_ipv4 *mask = ma ? nl_attr_get(ma) : NULL;

        format_ipv4(ds, "src", key->ipv4_src, MASK(mask, ipv4_src), verbose);
        format_ipv4(ds, "dst", key->ipv4_dst, MASK(mask, ipv4_dst), verbose);
        format_u8u(ds, "proto", key->ipv4_proto, MASK(mask, ipv4_proto),
                      verbose);
        format_u8x(ds, "tos", key->ipv4_tos, MASK(mask, ipv4_tos), verbose);
        format_u8u(ds, "ttl", key->ipv4_ttl, MASK(mask, ipv4_ttl), verbose);
        format_frag(ds, "frag", key->ipv4_frag, MASK(mask, ipv4_frag),
                    verbose);
        ds_chomp(ds, ',');
        break;
    }
    case OVS_KEY_ATTR_IPV6: {
        const struct ovs_key_ipv6 *key = nl_attr_get(a);
        const struct ovs_key_ipv6 *mask = ma ? nl_attr_get(ma) : NULL;

        format_ipv6(ds, "src", key->ipv6_src, MASK(mask, ipv6_src), verbose);
        format_ipv6(ds, "dst", key->ipv6_dst, MASK(mask, ipv6_dst), verbose);
        format_ipv6_label(ds, "label", key->ipv6_label, MASK(mask, ipv6_label),
                          verbose);
        format_u8u(ds, "proto", key->ipv6_proto, MASK(mask, ipv6_proto),
                      verbose);
        format_u8x(ds, "tclass", key->ipv6_tclass, MASK(mask, ipv6_tclass),
                      verbose);
        format_u8u(ds, "hlimit", key->ipv6_hlimit, MASK(mask, ipv6_hlimit),
                      verbose);
        format_frag(ds, "frag", key->ipv6_frag, MASK(mask, ipv6_frag),
                    verbose);
        ds_chomp(ds, ',');
        break;
    }
        /* These have the same structure and format. */
    case OVS_KEY_ATTR_TCP:
    case OVS_KEY_ATTR_UDP:
    case OVS_KEY_ATTR_SCTP: {
        const struct ovs_key_tcp *key = nl_attr_get(a);
        const struct ovs_key_tcp *mask = ma ? nl_attr_get(ma) : NULL;

        format_be16(ds, "src", key->tcp_src, MASK(mask, tcp_src), verbose);
        format_be16(ds, "dst", key->tcp_dst, MASK(mask, tcp_dst), verbose);
        ds_chomp(ds, ',');
        break;
    }
    case OVS_KEY_ATTR_TCP_FLAGS:
        if (!is_exact) {
            format_flags_masked(ds, NULL, packet_tcp_flag_to_string,
                                ntohs(nl_attr_get_be16(a)),
                                ntohs(nl_attr_get_be16(ma)));
        } else {
            format_flags(ds, packet_tcp_flag_to_string,
                         ntohs(nl_attr_get_be16(a)), ',');
        }
        break;

    case OVS_KEY_ATTR_ICMP: {
        const struct ovs_key_icmp *key = nl_attr_get(a);
        const struct ovs_key_icmp *mask = ma ? nl_attr_get(ma) : NULL;

        format_u8u(ds, "type", key->icmp_type, MASK(mask, icmp_type), verbose);
        format_u8u(ds, "code", key->icmp_code, MASK(mask, icmp_code), verbose);
        ds_chomp(ds, ',');
        break;
    }
    case OVS_KEY_ATTR_ICMPV6: {
        const struct ovs_key_icmpv6 *key = nl_attr_get(a);
        const struct ovs_key_icmpv6 *mask = ma ? nl_attr_get(ma) : NULL;

        format_u8u(ds, "type", key->icmpv6_type, MASK(mask, icmpv6_type),
                   verbose);
        format_u8u(ds, "code", key->icmpv6_code, MASK(mask, icmpv6_code),
                   verbose);
        ds_chomp(ds, ',');
        break;
    }
    case OVS_KEY_ATTR_ARP: {
        const struct ovs_key_arp *mask = ma ? nl_attr_get(ma) : NULL;
        const struct ovs_key_arp *key = nl_attr_get(a);

        format_ipv4(ds, "sip", key->arp_sip, MASK(mask, arp_sip), verbose);
        format_ipv4(ds, "tip", key->arp_tip, MASK(mask, arp_tip), verbose);
        format_be16(ds, "op", key->arp_op, MASK(mask, arp_op), verbose);
        format_eth(ds, "sha", key->arp_sha, MASK(mask, arp_sha), verbose);
        format_eth(ds, "tha", key->arp_tha, MASK(mask, arp_tha), verbose);
        ds_chomp(ds, ',');
        break;
    }
    case OVS_KEY_ATTR_ND: {
        const struct ovs_key_nd *mask = ma ? nl_attr_get(ma) : NULL;
        const struct ovs_key_nd *key = nl_attr_get(a);

        format_ipv6(ds, "target", key->nd_target, MASK(mask, nd_target),
                    verbose);
        format_eth(ds, "sll", key->nd_sll, MASK(mask, nd_sll), verbose);
        format_eth(ds, "tll", key->nd_tll, MASK(mask, nd_tll), verbose);

        ds_chomp(ds, ',');
        break;
    }
    case OVS_KEY_ATTR_UNSPEC:
    case __OVS_KEY_ATTR_MAX:
    default:
        format_generic_odp_key(a, ds);
        if (!is_exact) {
            ds_put_char(ds, '/');
            format_generic_odp_key(ma, ds);
        }
        break;
    }
    ds_put_char(ds, ')');
}

static struct nlattr *
generate_all_wildcard_mask(struct ofpbuf *ofp, const struct nlattr *key)
{
    const struct nlattr *a;
    unsigned int left;
    int type = nl_attr_type(key);
    int size = nl_attr_get_size(key);

    if (odp_flow_key_attr_len(type) >=0) {
        nl_msg_put_unspec_zero(ofp, type, size);
    } else {
        size_t nested_mask;

        nested_mask = nl_msg_start_nested(ofp, type);
        NL_ATTR_FOR_EACH(a, left, key, nl_attr_get_size(key)) {
            generate_all_wildcard_mask(ofp, nl_attr_get(a));
        }
        nl_msg_end_nested(ofp, nested_mask);
    }

    return ofpbuf_base(ofp);
}

/* Appends to 'ds' a string representation of the 'key_len' bytes of
 * OVS_KEY_ATTR_* attributes in 'key'. If non-null, additionally formats the
 * 'mask_len' bytes of 'mask' which apply to 'key'. If 'portno_names' is
 * non-null and 'verbose' is true, translates odp port number to its name. */
void
odp_flow_format(const struct nlattr *key, size_t key_len,
                const struct nlattr *mask, size_t mask_len,
                const struct hmap *portno_names, struct ds *ds, bool verbose)
{
    if (key_len) {
        const struct nlattr *a;
        unsigned int left;
        bool has_ethtype_key = false;
        const struct nlattr *ma = NULL;
        struct ofpbuf ofp;
        bool first_field = true;

        ofpbuf_init(&ofp, 100);
        NL_ATTR_FOR_EACH (a, left, key, key_len) {
            bool is_nested_attr;
            bool is_wildcard = false;
            int attr_type = nl_attr_type(a);

            if (attr_type == OVS_KEY_ATTR_ETHERTYPE) {
                has_ethtype_key = true;
            }

            is_nested_attr = (odp_flow_key_attr_len(attr_type) == -2);

            if (mask && mask_len) {
                ma = nl_attr_find__(mask, mask_len, nl_attr_type(a));
                is_wildcard = ma ? odp_mask_attr_is_wildcard(ma) : true;
            }

            if (verbose || !is_wildcard  || is_nested_attr) {
                if (is_wildcard && !ma) {
                    ma = generate_all_wildcard_mask(&ofp, a);
                }
                if (!first_field) {
                    ds_put_char(ds, ',');
                }
                format_odp_key_attr(a, ma, portno_names, ds, verbose);
                first_field = false;
            }
            ofpbuf_clear(&ofp);
        }
        ofpbuf_uninit(&ofp);

        if (left) {
            int i;

            if (left == key_len) {
                ds_put_cstr(ds, "<empty>");
            }
            ds_put_format(ds, ",***%u leftover bytes*** (", left);
            for (i = 0; i < left; i++) {
                ds_put_format(ds, "%02x", ((const uint8_t *) a)[i]);
            }
            ds_put_char(ds, ')');
        }
        if (!has_ethtype_key) {
            ma = nl_attr_find__(mask, mask_len, OVS_KEY_ATTR_ETHERTYPE);
            if (ma) {
                ds_put_format(ds, ",eth_type(0/0x%04"PRIx16")",
                              ntohs(nl_attr_get_be16(ma)));
            }
        }
    } else {
        ds_put_cstr(ds, "<empty>");
    }
}

/* Appends to 'ds' a string representation of the 'key_len' bytes of
 * OVS_KEY_ATTR_* attributes in 'key'. */
void
odp_flow_key_format(const struct nlattr *key,
                    size_t key_len, struct ds *ds)
{
    odp_flow_format(key, key_len, NULL, 0, NULL, ds, true);
}

static bool
ovs_frag_type_from_string(const char *s, enum ovs_frag_type *type)
{
    if (!strcasecmp(s, "no")) {
        *type = OVS_FRAG_TYPE_NONE;
    } else if (!strcasecmp(s, "first")) {
        *type = OVS_FRAG_TYPE_FIRST;
    } else if (!strcasecmp(s, "later")) {
        *type = OVS_FRAG_TYPE_LATER;
    } else {
        return false;
    }
    return true;
}

/* Parsing. */

static int
scan_eth(const char *s, uint8_t (*key)[ETH_ADDR_LEN],
         uint8_t (*mask)[ETH_ADDR_LEN])
{
    int n;

    if (ovs_scan(s, ETH_ADDR_SCAN_FMT"%n", ETH_ADDR_SCAN_ARGS(*key), &n)) {
        int len = n;

        if (mask) {
            if (ovs_scan(s + len, "/"ETH_ADDR_SCAN_FMT"%n",
                         ETH_ADDR_SCAN_ARGS(*mask), &n)) {
                len += n;
            } else {
                memset(mask, 0xff, sizeof *mask);
            }
        }
        return len;
    }
    return 0;
}

static int
scan_ipv4(const char *s, ovs_be32 *key, ovs_be32 *mask)
{
    int n;

    if (ovs_scan(s, IP_SCAN_FMT"%n", IP_SCAN_ARGS(key), &n)) {
        int len = n;

        if (mask) {
            if (ovs_scan(s + len, "/"IP_SCAN_FMT"%n",
                         IP_SCAN_ARGS(mask), &n)) {
                len += n;
            } else {
                *mask = OVS_BE32_MAX;
            }
        }
        return len;
    }
    return 0;
}

static int
scan_ipv6(const char *s, ovs_be32 (*key)[4], ovs_be32 (*mask)[4])
{
    int n;
    char ipv6_s[IPV6_SCAN_LEN + 1];

    if (ovs_scan(s, IPV6_SCAN_FMT"%n", ipv6_s, &n)
        && inet_pton(AF_INET6, ipv6_s, key) == 1) {
        int len = n;

        if (mask) {
            if (ovs_scan(s + len, "/"IPV6_SCAN_FMT"%n", ipv6_s, &n)
                && inet_pton(AF_INET6, ipv6_s, mask) == 1) {
                len += n;
            } else {
                memset(mask, 0xff, sizeof *mask);
            }
        }
        return len;
    }
    return 0;
}

static int
scan_ipv6_label(const char *s, ovs_be32 *key, ovs_be32 *mask)
{
    int key_, mask_;
    int n;

    if (ovs_scan(s, "%i%n", &key_, &n)
        && (key_ & ~IPV6_LABEL_MASK) == 0) {
        int len = n;

        *key = htonl(key_);
        if (mask) {
            if (ovs_scan(s + len, "/%i%n", &mask_, &n)
                && (mask_ & ~IPV6_LABEL_MASK) == 0) {
                len += n;
                *mask = htonl(mask_);
            } else {
                *mask = htonl(IPV6_LABEL_MASK);
            }
        }
        return len;
    }
    return 0;
}

static int
scan_u8(const char *s, uint8_t *key, uint8_t *mask)
{
    int n;

    if (ovs_scan(s, "%"SCNi8"%n", key, &n)) {
        int len = n;

        if (mask) {
            if (ovs_scan(s + len, "/%"SCNi8"%n", mask, &n)) {
                len += n;
            } else {
                *mask = UINT8_MAX;
            }
        }
        return len;
    }
    return 0;
}

static int
scan_u32(const char *s, uint32_t *key, uint32_t *mask)
{
    int n;

    if (ovs_scan(s, "%"SCNi32"%n", key, &n)) {
        int len = n;

        if (mask) {
            if (ovs_scan(s + len, "/%"SCNi32"%n", mask, &n)) {
                len += n;
            } else {
                *mask = UINT32_MAX;
            }
        }
        return len;
    }
    return 0;
}

static int
scan_be16(const char *s, ovs_be16 *key, ovs_be16 *mask)
{
    uint16_t key_, mask_;
    int n;

    if (ovs_scan(s, "%"SCNi16"%n", &key_, &n)) {
        int len = n;

        *key = htons(key_);
        if (mask) {
            if (ovs_scan(s + len, "/%"SCNi16"%n", &mask_, &n)) {
                len += n;
                *mask = htons(mask_);
            } else {
                *mask = OVS_BE16_MAX;
            }
        }
        return len;
    }
    return 0;
}

static int
scan_be64(const char *s, ovs_be64 *key, ovs_be64 *mask)
{
    uint64_t key_, mask_;
    int n;

    if (ovs_scan(s, "%"SCNi64"%n", &key_, &n)) {
        int len = n;

        *key = htonll(key_);
        if (mask) {
            if (ovs_scan(s + len, "/%"SCNi64"%n", &mask_, &n)) {
                len += n;
                *mask = htonll(mask_);
            } else {
                *mask = OVS_BE64_MAX;
            }
        }
        return len;
    }
    return 0;
}

static int
scan_tun_flags(const char *s, uint16_t *key, uint16_t *mask)
{
    uint32_t flags, fmask;
    int n;

    n = parse_flags(s, flow_tun_flag_to_string, &flags,
                    FLOW_TNL_F_MASK, mask ? &fmask : NULL);
    if (n >= 0 && s[n] == ')') {
        *key = flags;
        if (mask) {
            *mask = fmask;
        }
        return n + 1;
    }
    return 0;
}

static int
scan_tcp_flags(const char *s, ovs_be16 *key, ovs_be16 *mask)
{
    uint32_t flags, fmask;
    int n;

    n = parse_flags(s, packet_tcp_flag_to_string, &flags,
                    TCP_FLAGS(OVS_BE16_MAX), mask ? &fmask : NULL);
    if (n >= 0) {
        *key = htons(flags);
        if (mask) {
            *mask = htons(fmask);
        }
        return n;
    }
    return 0;
}

static int
scan_frag(const char *s, uint8_t *key, uint8_t *mask)
{
    int n;
    char frag[8];
    enum ovs_frag_type frag_type;

    if (ovs_scan(s, "%7[a-z]%n", frag, &n)
        && ovs_frag_type_from_string(frag, &frag_type)) {
        int len = n;

        *key = frag_type;
        if (mask) {
            *mask = UINT8_MAX;
        }
        return len;
    }
    return 0;
}

static int
scan_port(const char *s, uint32_t *key, uint32_t *mask,
          const struct simap *port_names)
{
    int n;

    if (ovs_scan(s, "%"SCNi32"%n", key, &n)) {
        int len = n;

        if (mask) {
            if (ovs_scan(s + len, "/%"SCNi32"%n", mask, &n)) {
                len += n;
            } else {
                *mask = UINT32_MAX;
            }
        }
        return len;
    } else if (port_names) {
        const struct simap_node *node;
        int len;

        len = strcspn(s, ")");
        node = simap_find_len(port_names, s, len);
        if (node) {
            *key = node->data;

            if (mask) {
                *mask = UINT32_MAX;
            }
            return len;
        }
    }
    return 0;
}

/* Helper for vlan parsing. */
struct ovs_key_vlan__ {
    ovs_be16 tci;
};

static bool
set_be16_bf(ovs_be16 *bf, uint8_t bits, uint8_t offset, uint16_t value)
{
    const uint16_t mask = ((1U << bits) - 1) << offset;

    if (value >> bits) {
        return false;
    }

    *bf = htons((ntohs(*bf) & ~mask) | (value << offset));
    return true;
}

static int
scan_be16_bf(const char *s, ovs_be16 *key, ovs_be16 *mask, uint8_t bits,
             uint8_t offset)
{
    uint16_t key_, mask_;
    int n;

    if (ovs_scan(s, "%"SCNi16"%n", &key_, &n)) {
        int len = n;

        if (set_be16_bf(key, bits, offset, key_)) {
            if (mask) {
                if (ovs_scan(s + len, "/%"SCNi16"%n", &mask_, &n)) {
                    len += n;

                    if (!set_be16_bf(mask, bits, offset, mask_)) {
                        return 0;
                    }
                } else {
                    *mask |= htons(((1U << bits) - 1) << offset);
                }
            }
            return len;
        }
    }
    return 0;
}

static int
scan_vid(const char *s, ovs_be16 *key, ovs_be16 *mask)
{
    return scan_be16_bf(s, key, mask, 12, VLAN_VID_SHIFT);
}

static int
scan_pcp(const char *s, ovs_be16 *key, ovs_be16 *mask)
{
    return scan_be16_bf(s, key, mask, 3, VLAN_PCP_SHIFT);
}

static int
scan_cfi(const char *s, ovs_be16 *key, ovs_be16 *mask)
{
    return scan_be16_bf(s, key, mask, 1, VLAN_CFI_SHIFT);
}

/* For MPLS. */
static bool
set_be32_bf(ovs_be32 *bf, uint8_t bits, uint8_t offset, uint32_t value)
{
    const uint32_t mask = ((1U << bits) - 1) << offset;

    if (value >> bits) {
        return false;
    }

    *bf = htonl((ntohl(*bf) & ~mask) | (value << offset));
    return true;
}

static int
scan_be32_bf(const char *s, ovs_be32 *key, ovs_be32 *mask, uint8_t bits,
             uint8_t offset)
{
    uint32_t key_, mask_;
    int n;

    if (ovs_scan(s, "%"SCNi32"%n", &key_, &n)) {
        int len = n;

        if (set_be32_bf(key, bits, offset, key_)) {
            if (mask) {
                if (ovs_scan(s + len, "/%"SCNi32"%n", &mask_, &n)) {
                    len += n;

                    if (!set_be32_bf(mask, bits, offset, mask_)) {
                        return 0;
                    }
                } else {
                    *mask |= htonl(((1U << bits) - 1) << offset);
                }
            }
            return len;
        }
    }
    return 0;
}

static int
scan_mpls_label(const char *s, ovs_be32 *key, ovs_be32 *mask)
{
    return scan_be32_bf(s, key, mask, 20, MPLS_LABEL_SHIFT);
}

static int
scan_mpls_tc(const char *s, ovs_be32 *key, ovs_be32 *mask)
{
    return scan_be32_bf(s, key, mask, 3, MPLS_TC_SHIFT);
}

static int
scan_mpls_ttl(const char *s, ovs_be32 *key, ovs_be32 *mask)
{
    return scan_be32_bf(s, key, mask, 8, MPLS_TTL_SHIFT);
}

static int
scan_mpls_bos(const char *s, ovs_be32 *key, ovs_be32 *mask)
{
    return scan_be32_bf(s, key, mask, 1, MPLS_BOS_SHIFT);
}

/* ATTR is compile-time constant, so only the case with correct data type
 * will be used.  However, the compiler complains about the data  type for
 * the other cases, so we must cast to make the compiler silent. */
#define SCAN_PUT_ATTR(BUF, ATTR, DATA)                          \
    if ((ATTR) == OVS_KEY_ATTR_TUNNEL) {                              \
        tun_key_to_attr(BUF, (const struct flow_tnl *)(void *)&(DATA)); \
    } else {                                                    \
        nl_msg_put_unspec(BUF, ATTR, &(DATA), sizeof (DATA));   \
    }

#define SCAN_IF(NAME)                           \
    if (strncmp(s, NAME, strlen(NAME)) == 0) {  \
        const char *start = s;                  \
        int len;                                \
                                                \
        s += strlen(NAME)

/* Usually no special initialization is needed. */
#define SCAN_BEGIN(NAME, TYPE)                  \
    SCAN_IF(NAME);                              \
        TYPE skey, smask;                       \
        memset(&skey, 0, sizeof skey);          \
        memset(&smask, 0, sizeof smask);        \
        do {                                    \
            len = 0;

/* VLAN needs special initialization. */
#define SCAN_BEGIN_INIT(NAME, TYPE, KEY_INIT, MASK_INIT)  \
    SCAN_IF(NAME);                                        \
        TYPE skey = KEY_INIT;                       \
        TYPE smask = MASK_INIT;                     \
        do {                                        \
            len = 0;

/* Scan unnamed entry as 'TYPE' */
#define SCAN_TYPE(TYPE, KEY, MASK)              \
    len = scan_##TYPE(s, KEY, MASK);            \
    if (len == 0) {                             \
        return -EINVAL;                         \
    }                                           \
    s += len

/* Scan named ('NAME') entry 'FIELD' as 'TYPE'. */
#define SCAN_FIELD(NAME, TYPE, FIELD)                                   \
    if (strncmp(s, NAME, strlen(NAME)) == 0) {                          \
        s += strlen(NAME);                                              \
        SCAN_TYPE(TYPE, &skey.FIELD, mask ? &smask.FIELD : NULL);       \
        continue;                                                       \
    }

#define SCAN_FINISH()                           \
        } while (*s++ == ',' && len != 0);      \
        if (s[-1] != ')') {                     \
            return -EINVAL;                     \
        }

#define SCAN_FINISH_SINGLE()                    \
        } while (false);                        \
        if (*s++ != ')') {                      \
            return -EINVAL;                     \
        }

#define SCAN_PUT(ATTR)                                  \
        if (!mask || !is_all_zeros(&smask, sizeof smask)) { \
            SCAN_PUT_ATTR(key, ATTR, skey);             \
            if (mask) {                                 \
                SCAN_PUT_ATTR(mask, ATTR, smask);       \
            }                                           \
        }

#define SCAN_END(ATTR)                                  \
        SCAN_FINISH();                                  \
        SCAN_PUT(ATTR);                                 \
        return s - start;                               \
    }

#define SCAN_END_SINGLE(ATTR)                           \
        SCAN_FINISH_SINGLE();                           \
        SCAN_PUT(ATTR);                                 \
        return s - start;                               \
    }

#define SCAN_SINGLE(NAME, TYPE, SCAN_AS, ATTR)       \
    SCAN_BEGIN(NAME, TYPE) {                         \
        SCAN_TYPE(SCAN_AS, &skey, &smask);           \
    } SCAN_END_SINGLE(ATTR)

#define SCAN_SINGLE_NO_MASK(NAME, TYPE, SCAN_AS, ATTR)       \
    SCAN_BEGIN(NAME, TYPE) {                         \
        SCAN_TYPE(SCAN_AS, &skey, NULL);           \
    } SCAN_END_SINGLE(ATTR)

/* scan_port needs one extra argument. */
#define SCAN_SINGLE_PORT(NAME, TYPE, ATTR)  \
    SCAN_BEGIN(NAME, TYPE) {                            \
        len = scan_port(s, &skey, &smask, port_names);  \
        if (len == 0) {                                 \
            return -EINVAL;                             \
        }                                               \
        s += len;                                       \
    } SCAN_END_SINGLE(ATTR)

static int
parse_odp_key_mask_attr(const char *s, const struct simap *port_names,
                        struct ofpbuf *key, struct ofpbuf *mask)
{
    SCAN_SINGLE("skb_priority(", uint32_t, u32, OVS_KEY_ATTR_PRIORITY);
    SCAN_SINGLE("skb_mark(", uint32_t, u32, OVS_KEY_ATTR_SKB_MARK);
    SCAN_SINGLE_NO_MASK("recirc_id(", uint32_t, u32, OVS_KEY_ATTR_RECIRC_ID);
    SCAN_SINGLE("dp_hash(", uint32_t, u32, OVS_KEY_ATTR_DP_HASH);

    SCAN_BEGIN("tunnel(", struct flow_tnl) {
        SCAN_FIELD("tun_id=", be64, tun_id);
        SCAN_FIELD("src=", ipv4, ip_src);
        SCAN_FIELD("dst=", ipv4, ip_dst);
        SCAN_FIELD("tos=", u8, ip_tos);
        SCAN_FIELD("ttl=", u8, ip_ttl);
        SCAN_FIELD("tp_src=", be16, tp_src);
        SCAN_FIELD("tp_dst=", be16, tp_dst);
        SCAN_FIELD("flags(", tun_flags, flags);
    } SCAN_END(OVS_KEY_ATTR_TUNNEL);

    SCAN_SINGLE_PORT("in_port(", uint32_t, OVS_KEY_ATTR_IN_PORT);

    SCAN_BEGIN("eth(", struct ovs_key_ethernet) {
        SCAN_FIELD("src=", eth, eth_src);
        SCAN_FIELD("dst=", eth, eth_dst);
    } SCAN_END(OVS_KEY_ATTR_ETHERNET);

    SCAN_BEGIN_INIT("vlan(", struct ovs_key_vlan__,
                    { htons(VLAN_CFI) }, { htons(VLAN_CFI) }) {
        SCAN_FIELD("vid=", vid, tci);
        SCAN_FIELD("pcp=", pcp, tci);
        SCAN_FIELD("cfi=", cfi, tci);
    } SCAN_END(OVS_KEY_ATTR_VLAN);

    SCAN_SINGLE("eth_type(", ovs_be16, be16, OVS_KEY_ATTR_ETHERTYPE);

    SCAN_BEGIN("mpls(", struct ovs_key_mpls) {
        SCAN_FIELD("label=", mpls_label, mpls_lse);
        SCAN_FIELD("tc=", mpls_tc, mpls_lse);
        SCAN_FIELD("ttl=", mpls_ttl, mpls_lse);
        SCAN_FIELD("bos=", mpls_bos, mpls_lse);
    } SCAN_END(OVS_KEY_ATTR_MPLS);

    SCAN_BEGIN("ipv4(", struct ovs_key_ipv4) {
        SCAN_FIELD("src=", ipv4, ipv4_src);
        SCAN_FIELD("dst=", ipv4, ipv4_dst);
        SCAN_FIELD("proto=", u8, ipv4_proto);
        SCAN_FIELD("tos=", u8, ipv4_tos);
        SCAN_FIELD("ttl=", u8, ipv4_ttl);
        SCAN_FIELD("frag=", frag, ipv4_frag);
    } SCAN_END(OVS_KEY_ATTR_IPV4);

    SCAN_BEGIN("ipv6(", struct ovs_key_ipv6) {
        SCAN_FIELD("src=", ipv6, ipv6_src);
        SCAN_FIELD("dst=", ipv6, ipv6_dst);
        SCAN_FIELD("label=", ipv6_label, ipv6_label);
        SCAN_FIELD("proto=", u8, ipv6_proto);
        SCAN_FIELD("tclass=", u8, ipv6_tclass);
        SCAN_FIELD("hlimit=", u8, ipv6_hlimit);
        SCAN_FIELD("frag=", frag, ipv6_frag);
    } SCAN_END(OVS_KEY_ATTR_IPV6);

    SCAN_BEGIN("tcp(", struct ovs_key_tcp) {
        SCAN_FIELD("src=", be16, tcp_src);
        SCAN_FIELD("dst=", be16, tcp_dst);
    } SCAN_END(OVS_KEY_ATTR_TCP);

    SCAN_SINGLE("tcp_flags(", ovs_be16, tcp_flags, OVS_KEY_ATTR_TCP_FLAGS);

    SCAN_BEGIN("udp(", struct ovs_key_udp) {
        SCAN_FIELD("src=", be16, udp_src);
        SCAN_FIELD("dst=", be16, udp_dst);
    } SCAN_END(OVS_KEY_ATTR_UDP);

    SCAN_BEGIN("sctp(", struct ovs_key_sctp) {
        SCAN_FIELD("src=", be16, sctp_src);
        SCAN_FIELD("dst=", be16, sctp_dst);
    } SCAN_END(OVS_KEY_ATTR_SCTP);

    SCAN_BEGIN("icmp(", struct ovs_key_icmp) {
        SCAN_FIELD("type=", u8, icmp_type);
        SCAN_FIELD("code=", u8, icmp_code);
    } SCAN_END(OVS_KEY_ATTR_ICMP);

    SCAN_BEGIN("icmpv6(", struct ovs_key_icmpv6) {
        SCAN_FIELD("type=", u8, icmpv6_type);
        SCAN_FIELD("code=", u8, icmpv6_code);
    } SCAN_END(OVS_KEY_ATTR_ICMPV6);

    SCAN_BEGIN("arp(", struct ovs_key_arp) {
        SCAN_FIELD("sip=", ipv4, arp_sip);
        SCAN_FIELD("tip=", ipv4, arp_tip);
        SCAN_FIELD("op=", be16, arp_op);
        SCAN_FIELD("sha=", eth, arp_sha);
        SCAN_FIELD("tha=", eth, arp_tha);
    } SCAN_END(OVS_KEY_ATTR_ARP);

    SCAN_BEGIN("nd(", struct ovs_key_nd) {
        SCAN_FIELD("target=", ipv6, nd_target);
        SCAN_FIELD("sll=", eth, nd_sll);
        SCAN_FIELD("tll=", eth, nd_tll);
    } SCAN_END(OVS_KEY_ATTR_ND);

    /* Encap open-coded. */
    if (!strncmp(s, "encap(", 6)) {
        const char *start = s;
        size_t encap, encap_mask = 0;

        encap = nl_msg_start_nested(key, OVS_KEY_ATTR_ENCAP);
        if (mask) {
            encap_mask = nl_msg_start_nested(mask, OVS_KEY_ATTR_ENCAP);
        }

        s += 6;
        for (;;) {
            int retval;

            s += strspn(s, ", \t\r\n");
            if (!*s) {
                return -EINVAL;
            } else if (*s == ')') {
                break;
            }

            retval = parse_odp_key_mask_attr(s, port_names, key, mask);
            if (retval < 0) {
                return retval;
            }
            s += retval;
        }
        s++;

        nl_msg_end_nested(key, encap);
        if (mask) {
            nl_msg_end_nested(mask, encap_mask);
        }

        return s - start;
    }

    return -EINVAL;
}

/* Parses the string representation of a datapath flow key, in the
 * format output by odp_flow_key_format().  Returns 0 if successful,
 * otherwise a positive errno value.  On success, the flow key is
 * appended to 'key' as a series of Netlink attributes.  On failure, no
 * data is appended to 'key'.  Either way, 'key''s data might be
 * reallocated.
 *
 * If 'port_names' is nonnull, it points to an simap that maps from a port name
 * to a port number.  (Port names may be used instead of port numbers in
 * in_port.)
 *
 * On success, the attributes appended to 'key' are individually syntactically
 * valid, but they may not be valid as a sequence.  'key' might, for example,
 * have duplicated keys.  odp_flow_key_to_flow() will detect those errors. */
int
odp_flow_from_string(const char *s, const struct simap *port_names,
                     struct ofpbuf *key, struct ofpbuf *mask)
{
    const size_t old_size = ofpbuf_size(key);
    for (;;) {
        int retval;

        s += strspn(s, delimiters);
        if (!*s) {
            return 0;
        }

        retval = parse_odp_key_mask_attr(s, port_names, key, mask);
        if (retval < 0) {
            ofpbuf_set_size(key, old_size);
            return -retval;
        }
        s += retval;
    }

    return 0;
}

static uint8_t
ovs_to_odp_frag(uint8_t nw_frag)
{
    return (nw_frag == 0 ? OVS_FRAG_TYPE_NONE
          : nw_frag == FLOW_NW_FRAG_ANY ? OVS_FRAG_TYPE_FIRST
          : OVS_FRAG_TYPE_LATER);
}

static uint8_t
ovs_to_odp_frag_mask(uint8_t nw_frag_mask)
{
    uint8_t frag_mask = ~(OVS_FRAG_TYPE_FIRST | OVS_FRAG_TYPE_LATER);

    frag_mask |= (nw_frag_mask & FLOW_NW_FRAG_ANY) ? OVS_FRAG_TYPE_FIRST : 0;
    frag_mask |= (nw_frag_mask & FLOW_NW_FRAG_LATER) ? OVS_FRAG_TYPE_LATER : 0;

    return frag_mask;
}

static void
odp_flow_key_from_flow__(struct ofpbuf *buf, const struct flow *flow,
                         const struct flow *mask, odp_port_t odp_in_port,
                         size_t max_mpls_depth, bool recirc, bool export_mask)
{
    struct ovs_key_ethernet *eth_key;
    size_t encap;
    const struct flow *data = export_mask ? mask : flow;

    nl_msg_put_u32(buf, OVS_KEY_ATTR_PRIORITY, data->skb_priority);

    if (flow->tunnel.ip_dst || export_mask) {
        tun_key_to_attr(buf, &data->tunnel);
    }

    nl_msg_put_u32(buf, OVS_KEY_ATTR_SKB_MARK, data->pkt_mark);

    if (recirc) {
        nl_msg_put_u32(buf, OVS_KEY_ATTR_RECIRC_ID, data->recirc_id);
        nl_msg_put_u32(buf, OVS_KEY_ATTR_DP_HASH, data->dp_hash);
    }

    /* Add an ingress port attribute if this is a mask or 'odp_in_port'
     * is not the magical value "ODPP_NONE". */
    if (export_mask || odp_in_port != ODPP_NONE) {
        nl_msg_put_odp_port(buf, OVS_KEY_ATTR_IN_PORT, odp_in_port);
    }

    eth_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_ETHERNET,
                                       sizeof *eth_key);
    memcpy(eth_key->eth_src, data->dl_src, ETH_ADDR_LEN);
    memcpy(eth_key->eth_dst, data->dl_dst, ETH_ADDR_LEN);

    if (flow->vlan_tci != htons(0) || flow->dl_type == htons(ETH_TYPE_VLAN)) {
        if (export_mask) {
            nl_msg_put_be16(buf, OVS_KEY_ATTR_ETHERTYPE, OVS_BE16_MAX);
        } else {
            nl_msg_put_be16(buf, OVS_KEY_ATTR_ETHERTYPE, htons(ETH_TYPE_VLAN));
        }
        nl_msg_put_be16(buf, OVS_KEY_ATTR_VLAN, data->vlan_tci);
        encap = nl_msg_start_nested(buf, OVS_KEY_ATTR_ENCAP);
        if (flow->vlan_tci == htons(0)) {
            goto unencap;
        }
    } else {
        encap = 0;
    }

    if (ntohs(flow->dl_type) < ETH_TYPE_MIN) {
        /* For backwards compatibility with kernels that don't support
         * wildcarding, the following convention is used to encode the
         * OVS_KEY_ATTR_ETHERTYPE for key and mask:
         *
         *   key      mask    matches
         * -------- --------  -------
         *  >0x5ff   0xffff   Specified Ethernet II Ethertype.
         *  >0x5ff      0     Any Ethernet II or non-Ethernet II frame.
         *  <none>   0xffff   Any non-Ethernet II frame (except valid
         *                    802.3 SNAP packet with valid eth_type).
         */
        if (export_mask) {
            nl_msg_put_be16(buf, OVS_KEY_ATTR_ETHERTYPE, OVS_BE16_MAX);
        }
        goto unencap;
    }

    nl_msg_put_be16(buf, OVS_KEY_ATTR_ETHERTYPE, data->dl_type);

    if (flow->dl_type == htons(ETH_TYPE_IP)) {
        struct ovs_key_ipv4 *ipv4_key;

        ipv4_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_IPV4,
                                            sizeof *ipv4_key);
        ipv4_key->ipv4_src = data->nw_src;
        ipv4_key->ipv4_dst = data->nw_dst;
        ipv4_key->ipv4_proto = data->nw_proto;
        ipv4_key->ipv4_tos = data->nw_tos;
        ipv4_key->ipv4_ttl = data->nw_ttl;
        ipv4_key->ipv4_frag = export_mask ? ovs_to_odp_frag_mask(data->nw_frag)
                                      : ovs_to_odp_frag(data->nw_frag);
    } else if (flow->dl_type == htons(ETH_TYPE_IPV6)) {
        struct ovs_key_ipv6 *ipv6_key;

        ipv6_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_IPV6,
                                            sizeof *ipv6_key);
        memcpy(ipv6_key->ipv6_src, &data->ipv6_src, sizeof ipv6_key->ipv6_src);
        memcpy(ipv6_key->ipv6_dst, &data->ipv6_dst, sizeof ipv6_key->ipv6_dst);
        ipv6_key->ipv6_label = data->ipv6_label;
        ipv6_key->ipv6_proto = data->nw_proto;
        ipv6_key->ipv6_tclass = data->nw_tos;
        ipv6_key->ipv6_hlimit = data->nw_ttl;
        ipv6_key->ipv6_frag = export_mask ? ovs_to_odp_frag_mask(data->nw_frag)
                                      : ovs_to_odp_frag(data->nw_frag);
    } else if (flow->dl_type == htons(ETH_TYPE_ARP) ||
               flow->dl_type == htons(ETH_TYPE_RARP)) {
        struct ovs_key_arp *arp_key;

        arp_key = nl_msg_put_unspec_zero(buf, OVS_KEY_ATTR_ARP,
                                         sizeof *arp_key);
        arp_key->arp_sip = data->nw_src;
        arp_key->arp_tip = data->nw_dst;
        arp_key->arp_op = htons(data->nw_proto);
        memcpy(arp_key->arp_sha, data->arp_sha, ETH_ADDR_LEN);
        memcpy(arp_key->arp_tha, data->arp_tha, ETH_ADDR_LEN);
    } else if (eth_type_mpls(flow->dl_type)) {
        struct ovs_key_mpls *mpls_key;
        int i, n;

        n = flow_count_mpls_labels(flow, NULL);
        n = MIN(n, max_mpls_depth);
        mpls_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_MPLS,
                                            n * sizeof *mpls_key);
        for (i = 0; i < n; i++) {
            mpls_key[i].mpls_lse = data->mpls_lse[i];
        }
    }

    if (is_ip_any(flow) && !(flow->nw_frag & FLOW_NW_FRAG_LATER)) {
        if (flow->nw_proto == IPPROTO_TCP) {
            struct ovs_key_tcp *tcp_key;

            tcp_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_TCP,
                                               sizeof *tcp_key);
            tcp_key->tcp_src = data->tp_src;
            tcp_key->tcp_dst = data->tp_dst;

            if (data->tcp_flags) {
                nl_msg_put_be16(buf, OVS_KEY_ATTR_TCP_FLAGS, data->tcp_flags);
            }
        } else if (flow->nw_proto == IPPROTO_UDP) {
            struct ovs_key_udp *udp_key;

            udp_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_UDP,
                                               sizeof *udp_key);
            udp_key->udp_src = data->tp_src;
            udp_key->udp_dst = data->tp_dst;
        } else if (flow->nw_proto == IPPROTO_SCTP) {
            struct ovs_key_sctp *sctp_key;

            sctp_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_SCTP,
                                               sizeof *sctp_key);
            sctp_key->sctp_src = data->tp_src;
            sctp_key->sctp_dst = data->tp_dst;
        } else if (flow->dl_type == htons(ETH_TYPE_IP)
                && flow->nw_proto == IPPROTO_ICMP) {
            struct ovs_key_icmp *icmp_key;

            icmp_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_ICMP,
                                                sizeof *icmp_key);
            icmp_key->icmp_type = ntohs(data->tp_src);
            icmp_key->icmp_code = ntohs(data->tp_dst);
        } else if (flow->dl_type == htons(ETH_TYPE_IPV6)
                && flow->nw_proto == IPPROTO_ICMPV6) {
            struct ovs_key_icmpv6 *icmpv6_key;

            icmpv6_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_ICMPV6,
                                                  sizeof *icmpv6_key);
            icmpv6_key->icmpv6_type = ntohs(data->tp_src);
            icmpv6_key->icmpv6_code = ntohs(data->tp_dst);

            if (flow->tp_dst == htons(0)
                && (flow->tp_src == htons(ND_NEIGHBOR_SOLICIT)
                    || flow->tp_src == htons(ND_NEIGHBOR_ADVERT))
                && (!export_mask || (data->tp_src == htons(0xffff)
                                     && data->tp_dst == htons(0xffff)))) {

                struct ovs_key_nd *nd_key;

                nd_key = nl_msg_put_unspec_uninit(buf, OVS_KEY_ATTR_ND,
                                                    sizeof *nd_key);
                memcpy(nd_key->nd_target, &data->nd_target,
                        sizeof nd_key->nd_target);
                memcpy(nd_key->nd_sll, data->arp_sha, ETH_ADDR_LEN);
                memcpy(nd_key->nd_tll, data->arp_tha, ETH_ADDR_LEN);
            }
        }
    }

unencap:
    if (encap) {
        nl_msg_end_nested(buf, encap);
    }
}

/* Appends a representation of 'flow' as OVS_KEY_ATTR_* attributes to 'buf'.
 * 'flow->in_port' is ignored (since it is likely to be an OpenFlow port
 * number rather than a datapath port number).  Instead, if 'odp_in_port'
 * is anything other than ODPP_NONE, it is included in 'buf' as the input
 * port.
 *
 * 'buf' must have at least ODPUTIL_FLOW_KEY_BYTES bytes of space, or be
 * capable of being expanded to allow for that much space.
 *
 * 'recirc' indicates support for recirculation fields. If this is true, then
 * these fields will always be serialised. */
void
odp_flow_key_from_flow(struct ofpbuf *buf, const struct flow *flow,
                       const struct flow *mask, odp_port_t odp_in_port,
                       bool recirc)
{
    odp_flow_key_from_flow__(buf, flow, mask, odp_in_port, SIZE_MAX, recirc,
                             false);
}

/* Appends a representation of 'mask' as OVS_KEY_ATTR_* attributes to
 * 'buf'.  'flow' is used as a template to determine how to interpret
 * 'mask'.  For example, the 'dl_type' of 'mask' describes the mask, but
 * it doesn't indicate whether the other fields should be interpreted as
 * ARP, IPv4, IPv6, etc.
 *
 * 'buf' must have at least ODPUTIL_FLOW_KEY_BYTES bytes of space, or be
 * capable of being expanded to allow for that much space.
 *
 * 'recirc' indicates support for recirculation fields. If this is true, then
 * these fields will always be serialised. */
void
odp_flow_key_from_mask(struct ofpbuf *buf, const struct flow *mask,
                       const struct flow *flow, uint32_t odp_in_port_mask,
                       size_t max_mpls_depth, bool recirc)
{
    odp_flow_key_from_flow__(buf, flow, mask, u32_to_odp(odp_in_port_mask),
                             max_mpls_depth, recirc, true);
}

/* Generate ODP flow key from the given packet metadata */
void
odp_key_from_pkt_metadata(struct ofpbuf *buf, const struct pkt_metadata *md)
{
    nl_msg_put_u32(buf, OVS_KEY_ATTR_PRIORITY, md->skb_priority);

    if (md->tunnel.ip_dst) {
        tun_key_to_attr(buf, &md->tunnel);
    }

    nl_msg_put_u32(buf, OVS_KEY_ATTR_SKB_MARK, md->pkt_mark);

    /* Add an ingress port attribute if 'odp_in_port' is not the magical
     * value "ODPP_NONE". */
    if (md->in_port.odp_port != ODPP_NONE) {
        nl_msg_put_odp_port(buf, OVS_KEY_ATTR_IN_PORT, md->in_port.odp_port);
    }
}

/* Generate packet metadata from the given ODP flow key. */
void
odp_key_to_pkt_metadata(const struct nlattr *key, size_t key_len,
                        struct pkt_metadata *md)
{
    const struct nlattr *nla;
    size_t left;
    uint32_t wanted_attrs = 1u << OVS_KEY_ATTR_PRIORITY |
        1u << OVS_KEY_ATTR_SKB_MARK | 1u << OVS_KEY_ATTR_TUNNEL |
        1u << OVS_KEY_ATTR_IN_PORT;

    *md = PKT_METADATA_INITIALIZER(ODPP_NONE);

    NL_ATTR_FOR_EACH (nla, left, key, key_len) {
        uint16_t type = nl_attr_type(nla);
        size_t len = nl_attr_get_size(nla);
        int expected_len = odp_flow_key_attr_len(type);

        if (len != expected_len && expected_len >= 0) {
            continue;
        }

        switch (type) {
        case OVS_KEY_ATTR_RECIRC_ID:
            md->recirc_id = nl_attr_get_u32(nla);
            wanted_attrs &= ~(1u << OVS_KEY_ATTR_RECIRC_ID);
            break;
        case OVS_KEY_ATTR_DP_HASH:
            md->dp_hash = nl_attr_get_u32(nla);
            wanted_attrs &= ~(1u << OVS_KEY_ATTR_DP_HASH);
            break;
        case OVS_KEY_ATTR_PRIORITY:
            md->skb_priority = nl_attr_get_u32(nla);
            wanted_attrs &= ~(1u << OVS_KEY_ATTR_PRIORITY);
            break;
        case OVS_KEY_ATTR_SKB_MARK:
            md->pkt_mark = nl_attr_get_u32(nla);
            wanted_attrs &= ~(1u << OVS_KEY_ATTR_SKB_MARK);
            break;
        case OVS_KEY_ATTR_TUNNEL: {
            enum odp_key_fitness res;

            res = odp_tun_key_from_attr(nla, &md->tunnel);
            if (res == ODP_FIT_ERROR) {
                memset(&md->tunnel, 0, sizeof md->tunnel);
            } else if (res == ODP_FIT_PERFECT) {
                wanted_attrs &= ~(1u << OVS_KEY_ATTR_TUNNEL);
            }
            break;
        }
        case OVS_KEY_ATTR_IN_PORT:
            md->in_port.odp_port = nl_attr_get_odp_port(nla);
            wanted_attrs &= ~(1u << OVS_KEY_ATTR_IN_PORT);
            break;
        default:
            break;
        }

        if (!wanted_attrs) {
            return; /* Have everything. */
        }
    }
}

uint32_t
odp_flow_key_hash(const struct nlattr *key, size_t key_len)
{
    BUILD_ASSERT_DECL(!(NLA_ALIGNTO % sizeof(uint32_t)));
    return hash_words(ALIGNED_CAST(const uint32_t *, key),
                      key_len / sizeof(uint32_t), 0);
}

static void
log_odp_key_attributes(struct vlog_rate_limit *rl, const char *title,
                       uint64_t attrs, int out_of_range_attr,
                       const struct nlattr *key, size_t key_len)
{
    struct ds s;
    int i;

    if (VLOG_DROP_DBG(rl)) {
        return;
    }

    ds_init(&s);
    for (i = 0; i < 64; i++) {
        if (attrs & (UINT64_C(1) << i)) {
            char namebuf[OVS_KEY_ATTR_BUFSIZE];

            ds_put_format(&s, " %s",
                          ovs_key_attr_to_string(i, namebuf, sizeof namebuf));
        }
    }
    if (out_of_range_attr) {
        ds_put_format(&s, " %d (and possibly others)", out_of_range_attr);
    }

    ds_put_cstr(&s, ": ");
    odp_flow_key_format(key, key_len, &s);

    VLOG_DBG("%s:%s", title, ds_cstr(&s));
    ds_destroy(&s);
}

static bool
odp_to_ovs_frag(uint8_t odp_frag, struct flow *flow)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

    if (odp_frag > OVS_FRAG_TYPE_LATER) {
        VLOG_ERR_RL(&rl, "invalid frag %"PRIu8" in flow key", odp_frag);
        return false;
    }

    if (odp_frag != OVS_FRAG_TYPE_NONE) {
        flow->nw_frag |= FLOW_NW_FRAG_ANY;
        if (odp_frag == OVS_FRAG_TYPE_LATER) {
            flow->nw_frag |= FLOW_NW_FRAG_LATER;
        }
    }
    return true;
}

static bool
parse_flow_nlattrs(const struct nlattr *key, size_t key_len,
                   const struct nlattr *attrs[], uint64_t *present_attrsp,
                   int *out_of_range_attrp)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(10, 10);
    const struct nlattr *nla;
    uint64_t present_attrs;
    size_t left;

    BUILD_ASSERT(OVS_KEY_ATTR_MAX < CHAR_BIT * sizeof present_attrs);
    present_attrs = 0;
    *out_of_range_attrp = 0;
    NL_ATTR_FOR_EACH (nla, left, key, key_len) {
        uint16_t type = nl_attr_type(nla);
        size_t len = nl_attr_get_size(nla);
        int expected_len = odp_flow_key_attr_len(type);

        if (len != expected_len && expected_len >= 0) {
            char namebuf[OVS_KEY_ATTR_BUFSIZE];

            VLOG_ERR_RL(&rl, "attribute %s has length %"PRIuSIZE" but should have "
                        "length %d", ovs_key_attr_to_string(type, namebuf,
                                                            sizeof namebuf),
                        len, expected_len);
            return false;
        }

        if (type > OVS_KEY_ATTR_MAX) {
            *out_of_range_attrp = type;
        } else {
            if (present_attrs & (UINT64_C(1) << type)) {
                char namebuf[OVS_KEY_ATTR_BUFSIZE];

                VLOG_ERR_RL(&rl, "duplicate %s attribute in flow key",
                            ovs_key_attr_to_string(type,
                                                   namebuf, sizeof namebuf));
                return false;
            }

            present_attrs |= UINT64_C(1) << type;
            attrs[type] = nla;
        }
    }
    if (left) {
        VLOG_ERR_RL(&rl, "trailing garbage in flow key");
        return false;
    }

    *present_attrsp = present_attrs;
    return true;
}

static enum odp_key_fitness
check_expectations(uint64_t present_attrs, int out_of_range_attr,
                   uint64_t expected_attrs,
                   const struct nlattr *key, size_t key_len)
{
    uint64_t missing_attrs;
    uint64_t extra_attrs;

    missing_attrs = expected_attrs & ~present_attrs;
    if (missing_attrs) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(10, 10);
        log_odp_key_attributes(&rl, "expected but not present",
                               missing_attrs, 0, key, key_len);
        return ODP_FIT_TOO_LITTLE;
    }

    extra_attrs = present_attrs & ~expected_attrs;
    if (extra_attrs || out_of_range_attr) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(10, 10);
        log_odp_key_attributes(&rl, "present but not expected",
                               extra_attrs, out_of_range_attr, key, key_len);
        return ODP_FIT_TOO_MUCH;
    }

    return ODP_FIT_PERFECT;
}

static bool
parse_ethertype(const struct nlattr *attrs[OVS_KEY_ATTR_MAX + 1],
                uint64_t present_attrs, uint64_t *expected_attrs,
                struct flow *flow, const struct flow *src_flow)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
    bool is_mask = flow != src_flow;

    if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_ETHERTYPE)) {
        flow->dl_type = nl_attr_get_be16(attrs[OVS_KEY_ATTR_ETHERTYPE]);
        if (!is_mask && ntohs(flow->dl_type) < ETH_TYPE_MIN) {
            VLOG_ERR_RL(&rl, "invalid Ethertype %"PRIu16" in flow key",
                        ntohs(flow->dl_type));
            return false;
        }
        if (is_mask && ntohs(src_flow->dl_type) < ETH_TYPE_MIN &&
            flow->dl_type != htons(0xffff)) {
            return false;
        }
        *expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_ETHERTYPE;
    } else {
        if (!is_mask) {
            flow->dl_type = htons(FLOW_DL_TYPE_NONE);
        } else if (ntohs(src_flow->dl_type) < ETH_TYPE_MIN) {
            /* See comments in odp_flow_key_from_flow__(). */
            VLOG_ERR_RL(&rl, "mask expected for non-Ethernet II frame");
            return false;
        }
    }
    return true;
}

static enum odp_key_fitness
parse_l2_5_onward(const struct nlattr *attrs[OVS_KEY_ATTR_MAX + 1],
                  uint64_t present_attrs, int out_of_range_attr,
                  uint64_t expected_attrs, struct flow *flow,
                  const struct nlattr *key, size_t key_len,
                  const struct flow *src_flow)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
    bool is_mask = src_flow != flow;
    const void *check_start = NULL;
    size_t check_len = 0;
    enum ovs_key_attr expected_bit = 0xff;

    if (eth_type_mpls(src_flow->dl_type)) {
        size_t size = nl_attr_get_size(attrs[OVS_KEY_ATTR_MPLS]);
        const ovs_be32 *mpls_lse = nl_attr_get(attrs[OVS_KEY_ATTR_MPLS]);
        int n = size / sizeof(ovs_be32);
        int i;

        if (!size || size % sizeof(ovs_be32)) {
            return ODP_FIT_ERROR;
        }

        if (!is_mask) {
            expected_attrs |= (UINT64_C(1) << OVS_KEY_ATTR_MPLS);

            if (!(present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_MPLS))) {
                return ODP_FIT_TOO_LITTLE;
            }
        } else if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_MPLS)) {
            if (flow->mpls_lse[0] && flow->dl_type != htons(0xffff)) {
                return ODP_FIT_ERROR;
            }
            expected_attrs |= (UINT64_C(1) << OVS_KEY_ATTR_MPLS);
        }

        for (i = 0; i < n && i < FLOW_MAX_MPLS_LABELS; i++) {
            flow->mpls_lse[i] = mpls_lse[i];
        }
        if (n > FLOW_MAX_MPLS_LABELS) {
            return ODP_FIT_TOO_MUCH;
        }

        if (!is_mask) {
            /* BOS may be set only in the innermost label. */
            for (i = 0; i < n - 1; i++) {
                if (flow->mpls_lse[i] & htonl(MPLS_BOS_MASK)) {
                    return ODP_FIT_ERROR;
                }
            }

            /* BOS must be set in the innermost label. */
            if (n < FLOW_MAX_MPLS_LABELS
                && !(flow->mpls_lse[n - 1] & htonl(MPLS_BOS_MASK))) {
                return ODP_FIT_TOO_LITTLE;
            }
        }

        goto done;
    } else if (src_flow->dl_type == htons(ETH_TYPE_IP)) {
        if (!is_mask) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_IPV4;
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_IPV4)) {
            const struct ovs_key_ipv4 *ipv4_key;

            ipv4_key = nl_attr_get(attrs[OVS_KEY_ATTR_IPV4]);
            flow->nw_src = ipv4_key->ipv4_src;
            flow->nw_dst = ipv4_key->ipv4_dst;
            flow->nw_proto = ipv4_key->ipv4_proto;
            flow->nw_tos = ipv4_key->ipv4_tos;
            flow->nw_ttl = ipv4_key->ipv4_ttl;
            if (is_mask) {
                flow->nw_frag = ipv4_key->ipv4_frag;
                check_start = ipv4_key;
                check_len = sizeof *ipv4_key;
                expected_bit = OVS_KEY_ATTR_IPV4;
            } else if (!odp_to_ovs_frag(ipv4_key->ipv4_frag, flow)) {
                return ODP_FIT_ERROR;
            }
        }
    } else if (src_flow->dl_type == htons(ETH_TYPE_IPV6)) {
        if (!is_mask) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_IPV6;
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_IPV6)) {
            const struct ovs_key_ipv6 *ipv6_key;

            ipv6_key = nl_attr_get(attrs[OVS_KEY_ATTR_IPV6]);
            memcpy(&flow->ipv6_src, ipv6_key->ipv6_src, sizeof flow->ipv6_src);
            memcpy(&flow->ipv6_dst, ipv6_key->ipv6_dst, sizeof flow->ipv6_dst);
            flow->ipv6_label = ipv6_key->ipv6_label;
            flow->nw_proto = ipv6_key->ipv6_proto;
            flow->nw_tos = ipv6_key->ipv6_tclass;
            flow->nw_ttl = ipv6_key->ipv6_hlimit;
            if (is_mask) {
                flow->nw_frag = ipv6_key->ipv6_frag;
                check_start = ipv6_key;
                check_len = sizeof *ipv6_key;
                expected_bit = OVS_KEY_ATTR_IPV6;
            } else if (!odp_to_ovs_frag(ipv6_key->ipv6_frag, flow)) {
                return ODP_FIT_ERROR;
            }
        }
    } else if (src_flow->dl_type == htons(ETH_TYPE_ARP) ||
               src_flow->dl_type == htons(ETH_TYPE_RARP)) {
        if (!is_mask) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_ARP;
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_ARP)) {
            const struct ovs_key_arp *arp_key;

            arp_key = nl_attr_get(attrs[OVS_KEY_ATTR_ARP]);
            flow->nw_src = arp_key->arp_sip;
            flow->nw_dst = arp_key->arp_tip;
            if (!is_mask && (arp_key->arp_op & htons(0xff00))) {
                VLOG_ERR_RL(&rl, "unsupported ARP opcode %"PRIu16" in flow "
                            "key", ntohs(arp_key->arp_op));
                return ODP_FIT_ERROR;
            }
            flow->nw_proto = ntohs(arp_key->arp_op);
            memcpy(flow->arp_sha, arp_key->arp_sha, ETH_ADDR_LEN);
            memcpy(flow->arp_tha, arp_key->arp_tha, ETH_ADDR_LEN);

            if (is_mask) {
                check_start = arp_key;
                check_len = sizeof *arp_key;
                expected_bit = OVS_KEY_ATTR_ARP;
            }
        }
    } else {
        goto done;
    }
    if (check_len > 0) { /* Happens only when 'is_mask'. */
        if (!is_all_zeros(check_start, check_len) &&
            flow->dl_type != htons(0xffff)) {
            return ODP_FIT_ERROR;
        } else {
            expected_attrs |= UINT64_C(1) << expected_bit;
        }
    }

    expected_bit = OVS_KEY_ATTR_UNSPEC;
    if (src_flow->nw_proto == IPPROTO_TCP
        && (src_flow->dl_type == htons(ETH_TYPE_IP) ||
            src_flow->dl_type == htons(ETH_TYPE_IPV6))
        && !(src_flow->nw_frag & FLOW_NW_FRAG_LATER)) {
        if (!is_mask) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_TCP;
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_TCP)) {
            const struct ovs_key_tcp *tcp_key;

            tcp_key = nl_attr_get(attrs[OVS_KEY_ATTR_TCP]);
            flow->tp_src = tcp_key->tcp_src;
            flow->tp_dst = tcp_key->tcp_dst;
            expected_bit = OVS_KEY_ATTR_TCP;
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_TCP_FLAGS)) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_TCP_FLAGS;
            flow->tcp_flags = nl_attr_get_be16(attrs[OVS_KEY_ATTR_TCP_FLAGS]);
        }
    } else if (src_flow->nw_proto == IPPROTO_UDP
               && (src_flow->dl_type == htons(ETH_TYPE_IP) ||
                   src_flow->dl_type == htons(ETH_TYPE_IPV6))
               && !(src_flow->nw_frag & FLOW_NW_FRAG_LATER)) {
        if (!is_mask) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_UDP;
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_UDP)) {
            const struct ovs_key_udp *udp_key;

            udp_key = nl_attr_get(attrs[OVS_KEY_ATTR_UDP]);
            flow->tp_src = udp_key->udp_src;
            flow->tp_dst = udp_key->udp_dst;
            expected_bit = OVS_KEY_ATTR_UDP;
        }
    } else if (src_flow->nw_proto == IPPROTO_SCTP
               && (src_flow->dl_type == htons(ETH_TYPE_IP) ||
                   src_flow->dl_type == htons(ETH_TYPE_IPV6))
               && !(src_flow->nw_frag & FLOW_NW_FRAG_LATER)) {
        if (!is_mask) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_SCTP;
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_SCTP)) {
            const struct ovs_key_sctp *sctp_key;

            sctp_key = nl_attr_get(attrs[OVS_KEY_ATTR_SCTP]);
            flow->tp_src = sctp_key->sctp_src;
            flow->tp_dst = sctp_key->sctp_dst;
            expected_bit = OVS_KEY_ATTR_SCTP;
        }
    } else if (src_flow->nw_proto == IPPROTO_ICMP
               && src_flow->dl_type == htons(ETH_TYPE_IP)
               && !(src_flow->nw_frag & FLOW_NW_FRAG_LATER)) {
        if (!is_mask) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_ICMP;
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_ICMP)) {
            const struct ovs_key_icmp *icmp_key;

            icmp_key = nl_attr_get(attrs[OVS_KEY_ATTR_ICMP]);
            flow->tp_src = htons(icmp_key->icmp_type);
            flow->tp_dst = htons(icmp_key->icmp_code);
            expected_bit = OVS_KEY_ATTR_ICMP;
        }
    } else if (src_flow->nw_proto == IPPROTO_ICMPV6
               && src_flow->dl_type == htons(ETH_TYPE_IPV6)
               && !(src_flow->nw_frag & FLOW_NW_FRAG_LATER)) {
        if (!is_mask) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_ICMPV6;
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_ICMPV6)) {
            const struct ovs_key_icmpv6 *icmpv6_key;

            icmpv6_key = nl_attr_get(attrs[OVS_KEY_ATTR_ICMPV6]);
            flow->tp_src = htons(icmpv6_key->icmpv6_type);
            flow->tp_dst = htons(icmpv6_key->icmpv6_code);
            expected_bit = OVS_KEY_ATTR_ICMPV6;
            if (src_flow->tp_dst == htons(0) &&
                (src_flow->tp_src == htons(ND_NEIGHBOR_SOLICIT) ||
                 src_flow->tp_src == htons(ND_NEIGHBOR_ADVERT))) {
                if (!is_mask) {
                    expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_ND;
                }
                if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_ND)) {
                    const struct ovs_key_nd *nd_key;

                    nd_key = nl_attr_get(attrs[OVS_KEY_ATTR_ND]);
                    memcpy(&flow->nd_target, nd_key->nd_target,
                           sizeof flow->nd_target);
                    memcpy(flow->arp_sha, nd_key->nd_sll, ETH_ADDR_LEN);
                    memcpy(flow->arp_tha, nd_key->nd_tll, ETH_ADDR_LEN);
                    if (is_mask) {
                        if (!is_all_zeros(nd_key, sizeof *nd_key) &&
                            (flow->tp_src != htons(0xffff) ||
                             flow->tp_dst != htons(0xffff))) {
                            return ODP_FIT_ERROR;
                        } else {
                            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_ND;
                        }
                    }
                }
            }
        }
    }
    if (is_mask && expected_bit != OVS_KEY_ATTR_UNSPEC) {
        if ((flow->tp_src || flow->tp_dst) && flow->nw_proto != 0xff) {
            return ODP_FIT_ERROR;
        } else {
            expected_attrs |= UINT64_C(1) << expected_bit;
        }
    }

done:
    return check_expectations(present_attrs, out_of_range_attr, expected_attrs,
                              key, key_len);
}

/* Parse 802.1Q header then encapsulated L3 attributes. */
static enum odp_key_fitness
parse_8021q_onward(const struct nlattr *attrs[OVS_KEY_ATTR_MAX + 1],
                   uint64_t present_attrs, int out_of_range_attr,
                   uint64_t expected_attrs, struct flow *flow,
                   const struct nlattr *key, size_t key_len,
                   const struct flow *src_flow)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
    bool is_mask = src_flow != flow;

    const struct nlattr *encap
        = (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_ENCAP)
           ? attrs[OVS_KEY_ATTR_ENCAP] : NULL);
    enum odp_key_fitness encap_fitness;
    enum odp_key_fitness fitness;

    /* Calculate fitness of outer attributes. */
    if (!is_mask) {
        expected_attrs |= ((UINT64_C(1) << OVS_KEY_ATTR_VLAN) |
                          (UINT64_C(1) << OVS_KEY_ATTR_ENCAP));
    } else {
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_VLAN)) {
            expected_attrs |= (UINT64_C(1) << OVS_KEY_ATTR_VLAN);
        }
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_ENCAP)) {
            expected_attrs |= (UINT64_C(1) << OVS_KEY_ATTR_ENCAP);
        }
    }
    fitness = check_expectations(present_attrs, out_of_range_attr,
                                 expected_attrs, key, key_len);

    /* Set vlan_tci.
     * Remove the TPID from dl_type since it's not the real Ethertype.  */
    flow->dl_type = htons(0);
    flow->vlan_tci = (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_VLAN)
                      ? nl_attr_get_be16(attrs[OVS_KEY_ATTR_VLAN])
                      : htons(0));
    if (!is_mask) {
        if (!(present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_VLAN))) {
            return ODP_FIT_TOO_LITTLE;
        } else if (flow->vlan_tci == htons(0)) {
            /* Corner case for a truncated 802.1Q header. */
            if (fitness == ODP_FIT_PERFECT && nl_attr_get_size(encap)) {
                return ODP_FIT_TOO_MUCH;
            }
            return fitness;
        } else if (!(flow->vlan_tci & htons(VLAN_CFI))) {
            VLOG_ERR_RL(&rl, "OVS_KEY_ATTR_VLAN 0x%04"PRIx16" is nonzero "
                        "but CFI bit is not set", ntohs(flow->vlan_tci));
            return ODP_FIT_ERROR;
        }
    } else {
        if (!(present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_ENCAP))) {
            return fitness;
        }
    }

    /* Now parse the encapsulated attributes. */
    if (!parse_flow_nlattrs(nl_attr_get(encap), nl_attr_get_size(encap),
                            attrs, &present_attrs, &out_of_range_attr)) {
        return ODP_FIT_ERROR;
    }
    expected_attrs = 0;

    if (!parse_ethertype(attrs, present_attrs, &expected_attrs, flow, src_flow)) {
        return ODP_FIT_ERROR;
    }
    encap_fitness = parse_l2_5_onward(attrs, present_attrs, out_of_range_attr,
                                      expected_attrs, flow, key, key_len,
                                      src_flow);

    /* The overall fitness is the worse of the outer and inner attributes. */
    return MAX(fitness, encap_fitness);
}

static enum odp_key_fitness
odp_flow_key_to_flow__(const struct nlattr *key, size_t key_len,
                       struct flow *flow, const struct flow *src_flow)
{
    const struct nlattr *attrs[OVS_KEY_ATTR_MAX + 1];
    uint64_t expected_attrs;
    uint64_t present_attrs;
    int out_of_range_attr;
    bool is_mask = src_flow != flow;

    memset(flow, 0, sizeof *flow);

    /* Parse attributes. */
    if (!parse_flow_nlattrs(key, key_len, attrs, &present_attrs,
                            &out_of_range_attr)) {
        return ODP_FIT_ERROR;
    }
    expected_attrs = 0;

    /* Metadata. */
    if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_RECIRC_ID)) {
        flow->recirc_id = nl_attr_get_u32(attrs[OVS_KEY_ATTR_RECIRC_ID]);
        expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_RECIRC_ID;
    } else if (is_mask) {
        /* Always exact match recirc_id if it is not specified. */
        flow->recirc_id = UINT32_MAX;
    }

    if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_DP_HASH)) {
        flow->dp_hash = nl_attr_get_u32(attrs[OVS_KEY_ATTR_DP_HASH]);
        expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_DP_HASH;
    }
    if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_PRIORITY)) {
        flow->skb_priority = nl_attr_get_u32(attrs[OVS_KEY_ATTR_PRIORITY]);
        expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_PRIORITY;
    }

    if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_SKB_MARK)) {
        flow->pkt_mark = nl_attr_get_u32(attrs[OVS_KEY_ATTR_SKB_MARK]);
        expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_SKB_MARK;
    }

    if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_TUNNEL)) {
        enum odp_key_fitness res;

        res = odp_tun_key_from_attr(attrs[OVS_KEY_ATTR_TUNNEL], &flow->tunnel);
        if (res == ODP_FIT_ERROR) {
            return ODP_FIT_ERROR;
        } else if (res == ODP_FIT_PERFECT) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_TUNNEL;
        }
    }

    if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_IN_PORT)) {
        flow->in_port.odp_port
            = nl_attr_get_odp_port(attrs[OVS_KEY_ATTR_IN_PORT]);
        expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_IN_PORT;
    } else if (!is_mask) {
        flow->in_port.odp_port = ODPP_NONE;
    }

    /* Ethernet header. */
    if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_ETHERNET)) {
        const struct ovs_key_ethernet *eth_key;

        eth_key = nl_attr_get(attrs[OVS_KEY_ATTR_ETHERNET]);
        memcpy(flow->dl_src, eth_key->eth_src, ETH_ADDR_LEN);
        memcpy(flow->dl_dst, eth_key->eth_dst, ETH_ADDR_LEN);
        if (is_mask) {
            expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_ETHERNET;
        }
    }
    if (!is_mask) {
        expected_attrs |= UINT64_C(1) << OVS_KEY_ATTR_ETHERNET;
    }

    /* Get Ethertype or 802.1Q TPID or FLOW_DL_TYPE_NONE. */
    if (!parse_ethertype(attrs, present_attrs, &expected_attrs, flow,
        src_flow)) {
        return ODP_FIT_ERROR;
    }

    if (is_mask
        ? (src_flow->vlan_tci & htons(VLAN_CFI)) != 0
        : src_flow->dl_type == htons(ETH_TYPE_VLAN)) {
        return parse_8021q_onward(attrs, present_attrs, out_of_range_attr,
                                  expected_attrs, flow, key, key_len, src_flow);
    }
    if (is_mask) {
        flow->vlan_tci = htons(0xffff);
        if (present_attrs & (UINT64_C(1) << OVS_KEY_ATTR_VLAN)) {
            flow->vlan_tci = nl_attr_get_be16(attrs[OVS_KEY_ATTR_VLAN]);
            expected_attrs |= (UINT64_C(1) << OVS_KEY_ATTR_VLAN);
        }
    }
    return parse_l2_5_onward(attrs, present_attrs, out_of_range_attr,
                             expected_attrs, flow, key, key_len, src_flow);
}

/* Converts the 'key_len' bytes of OVS_KEY_ATTR_* attributes in 'key' to a flow
 * structure in 'flow'.  Returns an ODP_FIT_* value that indicates how well
 * 'key' fits our expectations for what a flow key should contain.
 *
 * The 'in_port' will be the datapath's understanding of the port.  The
 * caller will need to translate with odp_port_to_ofp_port() if the
 * OpenFlow port is needed.
 *
 * This function doesn't take the packet itself as an argument because none of
 * the currently understood OVS_KEY_ATTR_* attributes require it.  Currently,
 * it is always possible to infer which additional attribute(s) should appear
 * by looking at the attributes for lower-level protocols, e.g. if the network
 * protocol in OVS_KEY_ATTR_IPV4 or OVS_KEY_ATTR_IPV6 is IPPROTO_TCP then we
 * know that a OVS_KEY_ATTR_TCP attribute must appear and that otherwise it
 * must be absent. */
enum odp_key_fitness
odp_flow_key_to_flow(const struct nlattr *key, size_t key_len,
                     struct flow *flow)
{
   return odp_flow_key_to_flow__(key, key_len, flow, flow);
}

/* Converts the 'key_len' bytes of OVS_KEY_ATTR_* attributes in 'key' to a mask
 * structure in 'mask'.  'flow' must be a previously translated flow
 * corresponding to 'mask'.  Returns an ODP_FIT_* value that indicates how well
 * 'key' fits our expectations for what a flow key should contain. */
enum odp_key_fitness
odp_flow_key_to_mask(const struct nlattr *key, size_t key_len,
                     struct flow *mask, const struct flow *flow)
{
   return odp_flow_key_to_flow__(key, key_len, mask, flow);
}

/* Returns 'fitness' as a string, for use in debug messages. */
const char *
odp_key_fitness_to_string(enum odp_key_fitness fitness)
{
    switch (fitness) {
    case ODP_FIT_PERFECT:
        return "OK";
    case ODP_FIT_TOO_MUCH:
        return "too_much";
    case ODP_FIT_TOO_LITTLE:
        return "too_little";
    case ODP_FIT_ERROR:
        return "error";
    default:
        return "<unknown>";
    }
}

/* Appends an OVS_ACTION_ATTR_USERSPACE action to 'odp_actions' that specifies
 * Netlink PID 'pid'.  If 'userdata' is nonnull, adds a userdata attribute
 * whose contents are the 'userdata_size' bytes at 'userdata' and returns the
 * offset within 'odp_actions' of the start of the cookie.  (If 'userdata' is
 * null, then the return value is not meaningful.) */
size_t
odp_put_userspace_action(uint32_t pid,
                         const void *userdata, size_t userdata_size,
                         odp_port_t tunnel_out_port,
                         struct ofpbuf *odp_actions)
{
    size_t userdata_ofs;
    size_t offset;

    offset = nl_msg_start_nested(odp_actions, OVS_ACTION_ATTR_USERSPACE);
    nl_msg_put_u32(odp_actions, OVS_USERSPACE_ATTR_PID, pid);
    if (userdata) {
        userdata_ofs = ofpbuf_size(odp_actions) + NLA_HDRLEN;

        /* The OVS kernel module before OVS 1.11 and the upstream Linux kernel
         * module before Linux 3.10 required the userdata to be exactly 8 bytes
         * long:
         *
         *   - The kernel rejected shorter userdata with -ERANGE.
         *
         *   - The kernel silently dropped userdata beyond the first 8 bytes.
         *
         * Thus, for maximum compatibility, always put at least 8 bytes.  (We
         * separately disable features that required more than 8 bytes.) */
        memcpy(nl_msg_put_unspec_zero(odp_actions, OVS_USERSPACE_ATTR_USERDATA,
                                      MAX(8, userdata_size)),
               userdata, userdata_size);
    } else {
        userdata_ofs = 0;
    }
    if (tunnel_out_port != ODPP_NONE) {
        nl_msg_put_odp_port(odp_actions, OVS_USERSPACE_ATTR_EGRESS_TUN_PORT,
                            tunnel_out_port);
    }
    nl_msg_end_nested(odp_actions, offset);

    return userdata_ofs;
}

void
odp_put_tunnel_action(const struct flow_tnl *tunnel,
                      struct ofpbuf *odp_actions)
{
    size_t offset = nl_msg_start_nested(odp_actions, OVS_ACTION_ATTR_SET);
    tun_key_to_attr(odp_actions, tunnel);
    nl_msg_end_nested(odp_actions, offset);
}

/* The commit_odp_actions() function and its helpers. */

static void
commit_set_action(struct ofpbuf *odp_actions, enum ovs_key_attr key_type,
                  const void *key, size_t key_size)
{
    size_t offset = nl_msg_start_nested(odp_actions, OVS_ACTION_ATTR_SET);
    nl_msg_put_unspec(odp_actions, key_type, key, key_size);
    nl_msg_end_nested(odp_actions, offset);
}

/* Masked set actions have a mask following the data within the netlink
 * attribute.  The unmasked bits in the data will be cleared as the data
 * is copied to the action. */
void
commit_masked_set_action(struct ofpbuf *odp_actions,
                         enum ovs_key_attr key_type,
                         const void *key_, const void *mask_, size_t key_size)
{
    size_t offset = nl_msg_start_nested(odp_actions,
                                        OVS_ACTION_ATTR_SET_MASKED);
    char *data = nl_msg_put_unspec_uninit(odp_actions, key_type, key_size * 2);
    const char *key = key_, *mask = mask_;

    memcpy(data + key_size, mask, key_size);
    /* Clear unmasked bits while copying. */
    while (key_size--) {
        *data++ = *key++ & *mask++;
    }
    nl_msg_end_nested(odp_actions, offset);
}

void
odp_put_pkt_mark_action(const uint32_t pkt_mark,
                        struct ofpbuf *odp_actions)
{
    commit_set_action(odp_actions, OVS_KEY_ATTR_SKB_MARK, &pkt_mark,
                      sizeof(pkt_mark));
}

/* If any of the flow key data that ODP actions can modify are different in
 * 'base->tunnel' and 'flow->tunnel', appends a set_tunnel ODP action to
 * 'odp_actions' that change the flow tunneling information in key from
 * 'base->tunnel' into 'flow->tunnel', and then changes 'base->tunnel' in the
 * same way.  In other words, operates the same as commit_odp_actions(), but
 * only on tunneling information. */
void
commit_odp_tunnel_action(const struct flow *flow, struct flow *base,
                         struct ofpbuf *odp_actions)
{
    /* A valid IPV4_TUNNEL must have non-zero ip_dst. */
    if (flow->tunnel.ip_dst) {
        if (!memcmp(&base->tunnel, &flow->tunnel, sizeof base->tunnel)) {
            return;
        }
        memcpy(&base->tunnel, &flow->tunnel, sizeof base->tunnel);
        odp_put_tunnel_action(&base->tunnel, odp_actions);
    }
}

static void
commit_set_ether_addr_action(const struct flow *flow, struct flow *base,
                             struct ofpbuf *odp_actions,
                             struct flow_wildcards *wc)
{
    struct ovs_key_ethernet eth_key;

    if (eth_addr_equals(base->dl_src, flow->dl_src) &&
        eth_addr_equals(base->dl_dst, flow->dl_dst)) {
        return;
    }

    memset(&wc->masks.dl_src, 0xff, sizeof wc->masks.dl_src);
    memset(&wc->masks.dl_dst, 0xff, sizeof wc->masks.dl_dst);

    memcpy(base->dl_src, flow->dl_src, ETH_ADDR_LEN);
    memcpy(base->dl_dst, flow->dl_dst, ETH_ADDR_LEN);

    memcpy(eth_key.eth_src, base->dl_src, ETH_ADDR_LEN);
    memcpy(eth_key.eth_dst, base->dl_dst, ETH_ADDR_LEN);

    commit_set_action(odp_actions, OVS_KEY_ATTR_ETHERNET,
                      &eth_key, sizeof(eth_key));
}

static void
pop_vlan(struct flow *base,
         struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    memset(&wc->masks.vlan_tci, 0xff, sizeof wc->masks.vlan_tci);

    if (base->vlan_tci & htons(VLAN_CFI)) {
        nl_msg_put_flag(odp_actions, OVS_ACTION_ATTR_POP_VLAN);
        base->vlan_tci = 0;
    }
}

static void
commit_vlan_action(ovs_be16 vlan_tci, struct flow *base,
                   struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    if (base->vlan_tci == vlan_tci) {
        return;
    }

    pop_vlan(base, odp_actions, wc);
    if (vlan_tci & htons(VLAN_CFI)) {
        struct ovs_action_push_vlan vlan;

        vlan.vlan_tpid = htons(ETH_TYPE_VLAN);
        vlan.vlan_tci = vlan_tci;
        nl_msg_put_unspec(odp_actions, OVS_ACTION_ATTR_PUSH_VLAN,
                          &vlan, sizeof vlan);
    }
    base->vlan_tci = vlan_tci;
}

static void
commit_mpls_action(const struct flow *flow, struct flow *base,
                   struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    int base_n = flow_count_mpls_labels(base, wc);
    int flow_n = flow_count_mpls_labels(flow, wc);
    int common_n = flow_count_common_mpls_labels(flow, flow_n, base, base_n,
                                                 wc);

    while (base_n > common_n) {
        if (base_n - 1 == common_n && flow_n > common_n) {
            /* If there is only one more LSE in base than there are common
             * between base and flow; and flow has at least one more LSE than
             * is common then the topmost LSE of base may be updated using
             * set */
            struct ovs_key_mpls mpls_key;

            mpls_key.mpls_lse = flow->mpls_lse[flow_n - base_n];
            commit_set_action(odp_actions, OVS_KEY_ATTR_MPLS,
                              &mpls_key, sizeof mpls_key);
            flow_set_mpls_lse(base, 0, mpls_key.mpls_lse);
            common_n++;
        } else {
            /* Otherwise, if there more LSEs in base than are common between
             * base and flow then pop the topmost one. */
            ovs_be16 dl_type;
            bool popped;

            /* If all the LSEs are to be popped and this is not the outermost
             * LSE then use ETH_TYPE_MPLS as the ethertype parameter of the
             * POP_MPLS action instead of flow->dl_type.
             *
             * This is because the POP_MPLS action requires its ethertype
             * argument to be an MPLS ethernet type but in this case
             * flow->dl_type will be a non-MPLS ethernet type.
             *
             * When the final POP_MPLS action occurs it use flow->dl_type and
             * the and the resulting packet will have the desired dl_type. */
            if ((!eth_type_mpls(flow->dl_type)) && base_n > 1) {
                dl_type = htons(ETH_TYPE_MPLS);
            } else {
                dl_type = flow->dl_type;
            }
            nl_msg_put_be16(odp_actions, OVS_ACTION_ATTR_POP_MPLS, dl_type);
            popped = flow_pop_mpls(base, base_n, flow->dl_type, wc);
            ovs_assert(popped);
            base_n--;
        }
    }

    /* If, after the above popping and setting, there are more LSEs in flow
     * than base then some LSEs need to be pushed. */
    while (base_n < flow_n) {
        struct ovs_action_push_mpls *mpls;

        mpls = nl_msg_put_unspec_zero(odp_actions,
                                      OVS_ACTION_ATTR_PUSH_MPLS,
                                      sizeof *mpls);
        mpls->mpls_ethertype = flow->dl_type;
        mpls->mpls_lse = flow->mpls_lse[flow_n - base_n - 1];
        flow_push_mpls(base, base_n, mpls->mpls_ethertype, wc);
        flow_set_mpls_lse(base, 0, mpls->mpls_lse);
        base_n++;
    }
}

static void
commit_set_ipv4_action(const struct flow *flow, struct flow *base,
                     struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    struct ovs_key_ipv4 ipv4_key;

    if (base->nw_src == flow->nw_src &&
        base->nw_dst == flow->nw_dst &&
        base->nw_tos == flow->nw_tos &&
        base->nw_ttl == flow->nw_ttl &&
        base->nw_frag == flow->nw_frag) {
        return;
    }

    memset(&wc->masks.nw_src, 0xff, sizeof wc->masks.nw_src);
    memset(&wc->masks.nw_dst, 0xff, sizeof wc->masks.nw_dst);
    memset(&wc->masks.nw_tos, 0xff, sizeof wc->masks.nw_tos);
    memset(&wc->masks.nw_ttl, 0xff, sizeof wc->masks.nw_ttl);
    memset(&wc->masks.nw_proto, 0xff, sizeof wc->masks.nw_proto);
    memset(&wc->masks.nw_frag, 0xff, sizeof wc->masks.nw_frag);

    ipv4_key.ipv4_src = base->nw_src = flow->nw_src;
    ipv4_key.ipv4_dst = base->nw_dst = flow->nw_dst;
    ipv4_key.ipv4_tos = base->nw_tos = flow->nw_tos;
    ipv4_key.ipv4_ttl = base->nw_ttl = flow->nw_ttl;
    ipv4_key.ipv4_proto = base->nw_proto;
    ipv4_key.ipv4_frag = ovs_to_odp_frag(base->nw_frag);

    commit_set_action(odp_actions, OVS_KEY_ATTR_IPV4,
                      &ipv4_key, sizeof(ipv4_key));
}

static void
commit_set_ipv6_action(const struct flow *flow, struct flow *base,
                       struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    struct ovs_key_ipv6 ipv6_key;

    if (ipv6_addr_equals(&base->ipv6_src, &flow->ipv6_src) &&
        ipv6_addr_equals(&base->ipv6_dst, &flow->ipv6_dst) &&
        base->ipv6_label == flow->ipv6_label &&
        base->nw_tos == flow->nw_tos &&
        base->nw_ttl == flow->nw_ttl &&
        base->nw_frag == flow->nw_frag) {
        return;
    }

    memset(&wc->masks.ipv6_src, 0xff, sizeof wc->masks.ipv6_src);
    memset(&wc->masks.ipv6_dst, 0xff, sizeof wc->masks.ipv6_dst);
    memset(&wc->masks.ipv6_label, 0xff, sizeof wc->masks.ipv6_label);
    memset(&wc->masks.nw_tos, 0xff, sizeof wc->masks.nw_tos);
    memset(&wc->masks.nw_ttl, 0xff, sizeof wc->masks.nw_ttl);
    memset(&wc->masks.nw_proto, 0xff, sizeof wc->masks.nw_proto);
    memset(&wc->masks.nw_frag, 0xff, sizeof wc->masks.nw_frag);

    base->ipv6_src = flow->ipv6_src;
    memcpy(&ipv6_key.ipv6_src, &base->ipv6_src, sizeof(ipv6_key.ipv6_src));
    base->ipv6_dst = flow->ipv6_dst;
    memcpy(&ipv6_key.ipv6_dst, &base->ipv6_dst, sizeof(ipv6_key.ipv6_dst));

    ipv6_key.ipv6_label = base->ipv6_label = flow->ipv6_label;
    ipv6_key.ipv6_tclass = base->nw_tos = flow->nw_tos;
    ipv6_key.ipv6_hlimit = base->nw_ttl = flow->nw_ttl;
    ipv6_key.ipv6_proto = base->nw_proto;
    ipv6_key.ipv6_frag = ovs_to_odp_frag(base->nw_frag);

    commit_set_action(odp_actions, OVS_KEY_ATTR_IPV6,
                      &ipv6_key, sizeof(ipv6_key));
}

static enum slow_path_reason
commit_set_arp_action(const struct flow *flow, struct flow *base,
                      struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    struct ovs_key_arp arp_key;

    if (base->nw_src == flow->nw_src &&
        base->nw_dst == flow->nw_dst &&
        base->nw_proto == flow->nw_proto &&
        eth_addr_equals(base->arp_sha, flow->arp_sha) &&
        eth_addr_equals(base->arp_tha, flow->arp_tha)) {
        return 0;
    }

    memset(&wc->masks.nw_src, 0xff, sizeof wc->masks.nw_src);
    memset(&wc->masks.nw_dst, 0xff, sizeof wc->masks.nw_dst);
    memset(&wc->masks.nw_proto, 0xff, sizeof wc->masks.nw_proto);
    memset(&wc->masks.arp_sha, 0xff, sizeof wc->masks.arp_sha);
    memset(&wc->masks.arp_tha, 0xff, sizeof wc->masks.arp_tha);

    base->nw_src = flow->nw_src;
    base->nw_dst = flow->nw_dst;
    base->nw_proto = flow->nw_proto;
    memcpy(base->arp_sha, flow->arp_sha, ETH_ADDR_LEN);
    memcpy(base->arp_tha, flow->arp_tha, ETH_ADDR_LEN);

    arp_key.arp_sip = base->nw_src;
    arp_key.arp_tip = base->nw_dst;
    arp_key.arp_op = htons(base->nw_proto);
    memcpy(arp_key.arp_sha, flow->arp_sha, ETH_ADDR_LEN);
    memcpy(arp_key.arp_tha, flow->arp_tha, ETH_ADDR_LEN);

    commit_set_action(odp_actions, OVS_KEY_ATTR_ARP, &arp_key, sizeof arp_key);

    return SLOW_ACTION;
}

static enum slow_path_reason
commit_set_nw_action(const struct flow *flow, struct flow *base,
                     struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    /* Check if 'flow' really has an L3 header. */
    if (!flow->nw_proto) {
        return 0;
    }

    switch (ntohs(base->dl_type)) {
    case ETH_TYPE_IP:
        commit_set_ipv4_action(flow, base, odp_actions, wc);
        break;

    case ETH_TYPE_IPV6:
        commit_set_ipv6_action(flow, base, odp_actions, wc);
        break;

    case ETH_TYPE_ARP:
        return commit_set_arp_action(flow, base, odp_actions, wc);
    }

    return 0;
}

static void
commit_set_port_action(const struct flow *flow, struct flow *base,
                       struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    /* Check if 'flow' really has an L3 header. */
    if (!flow->nw_proto) {
        return;
    }

    if (!is_ip_any(base) || (!base->tp_src && !base->tp_dst)) {
        return;
    }

    if (base->tp_src == flow->tp_src &&
        base->tp_dst == flow->tp_dst) {
        return;
    }

    memset(&wc->masks.tp_src, 0xff, sizeof wc->masks.tp_src);
    memset(&wc->masks.tp_dst, 0xff, sizeof wc->masks.tp_dst);

    if (flow->nw_proto == IPPROTO_TCP) {
        struct ovs_key_tcp port_key;

        port_key.tcp_src = base->tp_src = flow->tp_src;
        port_key.tcp_dst = base->tp_dst = flow->tp_dst;

        commit_set_action(odp_actions, OVS_KEY_ATTR_TCP,
                          &port_key, sizeof(port_key));

    } else if (flow->nw_proto == IPPROTO_UDP) {
        struct ovs_key_udp port_key;

        port_key.udp_src = base->tp_src = flow->tp_src;
        port_key.udp_dst = base->tp_dst = flow->tp_dst;

        commit_set_action(odp_actions, OVS_KEY_ATTR_UDP,
                          &port_key, sizeof(port_key));
    } else if (flow->nw_proto == IPPROTO_SCTP) {
        struct ovs_key_sctp port_key;

        port_key.sctp_src = base->tp_src = flow->tp_src;
        port_key.sctp_dst = base->tp_dst = flow->tp_dst;

        commit_set_action(odp_actions, OVS_KEY_ATTR_SCTP,
                          &port_key, sizeof(port_key));
    }
}

static void
commit_set_priority_action(const struct flow *flow, struct flow *base,
                           struct ofpbuf *odp_actions,
                           struct flow_wildcards *wc)
{
    if (base->skb_priority == flow->skb_priority) {
        return;
    }

    memset(&wc->masks.skb_priority, 0xff, sizeof wc->masks.skb_priority);
    base->skb_priority = flow->skb_priority;

    commit_set_action(odp_actions, OVS_KEY_ATTR_PRIORITY,
                      &base->skb_priority, sizeof(base->skb_priority));
}

static void
commit_set_pkt_mark_action(const struct flow *flow, struct flow *base,
                           struct ofpbuf *odp_actions,
                           struct flow_wildcards *wc)
{
    if (base->pkt_mark == flow->pkt_mark) {
        return;
    }

    memset(&wc->masks.pkt_mark, 0xff, sizeof wc->masks.pkt_mark);
    base->pkt_mark = flow->pkt_mark;

    odp_put_pkt_mark_action(base->pkt_mark, odp_actions);
}

/* If any of the flow key data that ODP actions can modify are different in
 * 'base' and 'flow', appends ODP actions to 'odp_actions' that change the flow
 * key from 'base' into 'flow', and then changes 'base' the same way.  Does not
 * commit set_tunnel actions.  Users should call commit_odp_tunnel_action()
 * in addition to this function if needed.  Sets fields in 'wc' that are
 * used as part of the action.
 *
 * Returns a reason to force processing the flow's packets into the userspace
 * slow path, if there is one, otherwise 0. */
enum slow_path_reason
commit_odp_actions(const struct flow *flow, struct flow *base,
                   struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    enum slow_path_reason slow;

    commit_set_ether_addr_action(flow, base, odp_actions, wc);
    slow = commit_set_nw_action(flow, base, odp_actions, wc);
    commit_set_port_action(flow, base, odp_actions, wc);
    commit_mpls_action(flow, base, odp_actions, wc);
    commit_vlan_action(flow->vlan_tci, base, odp_actions, wc);
    commit_set_priority_action(flow, base, odp_actions, wc);
    commit_set_pkt_mark_action(flow, base, odp_actions, wc);

    return slow;
}
