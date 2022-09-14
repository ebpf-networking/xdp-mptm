/* SPDX-License-Identifier: GPL-2->0 
 *  
 * Authors:
 * Dushyant Behl <dushyantbehl@in.ibm.com>
 * Sayandeep Sen <sayandes@in.ibm.com>
 * Palanivel Kodeswaran <palani.kodeswaran@in.ibm.com>
 */

#include <linux/bpf.h>
#include <linux/in.h>

#include <linux/if_ether.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include <common/parsing_helpers.h>

#include <kernel/lib/pkt-parse.h>
#include <kernel/lib/pkt-encap.h>
#include <kernel/lib/geneve.h>
#include <kernel/lib/map-defs.h>

/* Defines xdp_stats_map */
#include <common/xdp_stats_kern_user.h>
#include <common/xdp_stats_kern.h>

/* Inspired from Katran.
 * ETH_P_IP and ETH_P_IPV6 in Big Endian format.
 * So we don't have to do htons on each packet
 */
#define BE_ETH_P_IP   0x0008
#define BE_ETH_P_IPV6 0xDD88
#define BE_ETH_P_ARP  0x0608

#define MAX_ENTRIES 1024

struct bpf_map_def SEC("maps") mptm_tunnel_info_map = {
    .type        = BPF_MAP_TYPE_HASH,
    .key_size    = sizeof(tunnel_map_key_t),
    .value_size  = sizeof(mptm_tunnel_info),
    .max_entries = MAX_ENTRIES,
};

struct bpf_map_def SEC("maps") mptm_tunnel_redirect_map = {
    .type        = BPF_MAP_TYPE_HASH,
    .key_size    = sizeof(redirect_map_key_t),
    .value_size  = sizeof(__u32),
    .max_entries = MAX_ENTRIES*2,
};

struct bpf_map_def SEC("maps") mptm_tunnel_redirect_if_devmap = {
    .type        = BPF_MAP_TYPE_DEVMAP,
    .key_size    = sizeof(__u32),
    .value_size  = sizeof(__u32),
    .max_entries = MAX_ENTRIES*2,
};

SEC("mptm_push")
int mptm_xdp_tunnel_push(struct xdp_md *ctx) {
    int action = XDP_PASS;  //default action

    /* header pointers */
    struct ethhdr *eth;
    struct iphdr *ip;

    /* map values and tunnel informations */
    struct tunnel_info* tn;
    tunnel_map_key_t key;
    __u8 tun_type;

    void *data = (void *)((long)ctx->data);
    void *data_end = (void *)((long)ctx->data_end);

    if (parse_pkt_headers(data, data_end, &eth, &ip, NULL) != 0) {
        goto out;
    }

    key.s_addr = ip->saddr;
    key.d_addr = ip->daddr;

    tn = bpf_map_lookup_elem(&mptm_tunnel_info_map, &key);
    if(tn == NULL) {
      mptm_print("[ERR] map entry missing for key %d\n", key);
      goto out;
    }

    tun_type = tn->tunnel_type;
    if (tun_type == VLAN) {
        action = trigger_vlan_push(ctx, eth, tn);
    }
    else if (tun_type == GENEVE) {
        action = trigger_geneve_push(ctx, eth, tn);
    } else {
        bpf_debug("[ERR] tunnel type is unknown");
        goto out;
    }

    if (tn->redirect) {
        __u64 flags = 0; // keep redirect flags zero for now
        __u32 *counter;

        redirect_map_key_t redirect_key = ip->daddr;
        counter = bpf_map_lookup_elem(&mptm_tunnel_redirect_map, &redirect_key);
        if(counter == NULL) {
            bpf_debug("[ERR] map entry missing for redirect key %d\n", redirect_key);
            goto out;
        }

        action = bpf_redirect_map(&mptm_tunnel_redirect_if_devmap, *counter, flags);
    }

  out:
    return xdp_stats_record_action(ctx, action);
}

SEC("mptm_pop")
int mptm_xdp_tunnel_pop(struct xdp_md *ctx) {
    int action = XDP_PASS;  //default action

    /*
       If packet is ENCAPSULATED
       Check packet tunnel - VLAN? GENEVE? VXLAN? ETC?
       check inner destination of packet
       use inner destination ip as the key in the tunnel iface map
       if present then do decap and send to the ingress interface present
       in the tunnel map
    */

    /* header pointers */
    struct ethhdr *eth;
    struct iphdr *ip;
    struct udphdr *udp;

    /* map values and tunnel informations */
    struct tunnel_info* tn;
    tunnel_map_key_t key;
    __u8 tun_type;

    void *data = (void *)((long)ctx->data);
    void *data_end = (void *)((long)ctx->data_end);

    if (parse_pkt_headers(data, data_end, &eth, &ip, &udp) != 0)
        goto out;

    if (udphdr->dest == BE_GEN_DSTPORT) {
        // GENEVE packet
        // Check inner packet if there is a rule corresponding to
        // inner source which will be source for us as we received the packet
        int outer_hdr_size = sizeof(struct genevehdr) +
                             sizeof(struct udphdr) +
                             sizeof(struct iphdr) +
                             sizeof(struct ethhdr);
        long ret = bpf_xdp_adjust_head(ctx, outer_hdr_size);
        if (ret != 0l) {
            mptm_print("[Agent:] DROP (BUG): Failure adjusting packet header!\n");
            return XDP_DROP;
        }

        /* recalculate the data pointers */
        data = (void *)(long)ctx->data;
        data_end = (void *)(long)ctx->data_end;

        /* header pointers */
        struct ethhdr *inner_eth;
        struct iphdr *inner_ip;

        if (parse_pkt_headers(data, data_end, &inner_eth, &inner_ip, NULL) != 0)
            goto out;

        /* map values and tunnel informations */
        struct tunnel_info* tn;
        tunnel_map_key_t key;
        __u8 tun_type;

        /* Packet is coming from outside so source and dest must be inversed */
        key.s_addr = ip->saddr;
        key.d_addr = ip->daddr;

        tn = bpf_map_lookup_elem(&mptm_tunnel_info_map, &key);
        if(tn == NULL) {
            mptm_print("[ERR] map entry missing for key %d\n", key);
            goto out;
        }

        tun_type = tn->tunnel_type;
        if (tun_type != GENEVE) {
            mptm_print("Packet is changed but did not belong to us!");
            return XDP_DROP;
        }

        __u64 flags = 0; // keep redirect flags zero for now
        __u32 *counter;

        redirect_map_key_t redirect_key = ip->daddr;
        counter = bpf_map_lookup_elem(&mptm_tunnel_redirect_map, &redirect_key);
        if(counter == NULL) {
            bpf_debug("[ERR] map entry missing for inverse redirect key %d\n", redirect_key);
            goto out;
        }

        action = bpf_redirect_map(&mptm_tunnel_redirect_if_devmap, *counter, flags);
    }

    goto out;

  out:
    return xdp_stats_record_action(ctx, action);
}

char _license[] SEC("license") = "GPL";
