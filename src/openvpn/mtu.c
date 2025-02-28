/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2021 OpenVPN Inc <sales@openvpn.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_MSC_VER)
#include "config-msvc.h"
#endif

#include "syshead.h"

#include "common.h"
#include "buffer.h"
#include "error.h"
#include "integer.h"
#include "mtu.h"
#include "options.h"
#include "crypto.h"

#include "memdbg.h"

/* allocate a buffer for socket or tun layer */
void
alloc_buf_sock_tun(struct buffer *buf,
                   const struct frame *frame,
                   const bool tuntap_buffer)
{
    /* allocate buffer for overlapped I/O */
    *buf = alloc_buf(BUF_SIZE(frame));
    ASSERT(buf_init(buf, FRAME_HEADROOM(frame)));
    buf->len = tuntap_buffer ? MAX_RW_SIZE_TUN(frame) : MAX_RW_SIZE_LINK(frame);
    ASSERT(buf_safe(buf, 0));
}

size_t
frame_calculate_protocol_header_size(const struct key_type *kt,
                                     const struct options *options,
                                     unsigned int payload_size,
                                     bool occ)
{
    /* Sum of all the overhead that reduces the usable packet size */
    size_t header_size = 0;

    bool tlsmode = options->tls_server || options->tls_client;

    /* A socks proxy adds 10 byte of extra header to each packet
     * (we only support Socks with IPv4, this value is different for IPv6) */
    if (options->ce.socks_proxy_server && proto_is_udp(options->ce.proto))
    {
        header_size += 10;
    }

    /* TCP stream based packets have a 16 bit length field */
    if (proto_is_tcp(options->ce.proto))
    {
        header_size += 2;
    }

    /* Add the opcode and peerid */
    if (tlsmode)
    {
        header_size += options->use_peer_id ? 4 : 1;
    }

    /* Add the crypto overhead */
    bool packet_id = options->replay;
    bool packet_id_long_form = !tlsmode || cipher_kt_mode_ofb_cfb(kt->cipher);

    /* For figuring out the crypto overhead, we need the size of the payload
     * including all headers that also get encrypted as part of the payload */
    header_size += calculate_crypto_overhead(kt, packet_id,
                                             packet_id_long_form,
                                             payload_size, occ);
    return header_size;
}


size_t
frame_calculate_payload_overhead(const struct frame *frame,
                                 const struct options *options,
                                 bool extra_tun)
{
    size_t overhead = 0;

    /* This is the overhead of tap device that is not included in the MTU itself
     * i.e. Ethernet header that we still need to transmit as part of the
     * payload*/
    if (extra_tun)
    {
        overhead += frame->extra_tun;
    }

#if defined(USE_COMP)
    /* v1 Compression schemes add 1 byte header. V2 only adds a header when it
     * does not increase the packet length. We ignore the unlikely escaping
     * for tap here */
    if (options->comp.alg == COMP_ALG_LZ4 || options->comp.alg == COMP_ALG_STUB
        || options->comp.alg == COMP_ALG_LZO)
    {
        overhead += 1;
    }
#endif
#if defined(ENABLE_FRAGMENT)
    /* Add the size of the fragment header (uint32_t) */
    if (options->ce.fragment)
    {
        overhead += 4;
    }
#endif
    return overhead;
}

size_t
frame_calculate_payload_size(const struct frame *frame, const struct options *options)
{
    size_t payload_size = options->ce.tun_mtu;
    payload_size += frame_calculate_payload_overhead(frame, options, true);
    return payload_size;
}

size_t
calc_options_string_link_mtu(const struct options *o, const struct frame *frame)
{
    unsigned int payload = frame_calculate_payload_size(frame, o);

    /* neither --secret nor TLS mode */
    if (!o->tls_client && !o->tls_server && !o->shared_secret_file)
    {
        return payload;
    }

    struct key_type occ_kt;

    /* o->ciphername might be BF-CBC even though the underlying SSL library
     * does not support it. For this reason we workaround this corner case
     * by pretending to have no encryption enabled and by manually adding
     * the required packet overhead to the MTU computation.
     */
    const char* ciphername = o->ciphername;

    unsigned int overhead = 0;

    if (strcmp(o->ciphername, "BF-CBC") == 0)
    {
        /* none has no overhead, so use this to later add only --auth
         * overhead */

        /* overhead of BF-CBC: 64 bit block size, 64 bit IV size */
        overhead += 64/8 + 64/8;
        /* set ciphername to none, so its size does get added in the
         * fake_kt and the cipher is not tried to be resolved */
        ciphername = "none";
    }

    /* We pass tlsmode always true here since as we do not need to check if
     * the ciphers are actually valid for non tls in occ calucation */
    init_key_type(&occ_kt, ciphername, o->authname, true, false);

    overhead += frame_calculate_protocol_header_size(&occ_kt, o, 0, true);

    return payload + overhead;
}

void
frame_finalize(struct frame *frame,
               bool link_mtu_defined,
               int link_mtu,
               bool tun_mtu_defined,
               int tun_mtu)
{
    /* Set link_mtu based on command line options */
    if (tun_mtu_defined)
    {
        ASSERT(!link_mtu_defined);
        frame->link_mtu = tun_mtu + TUN_LINK_DELTA(frame);
    }
    else
    {
        ASSERT(link_mtu_defined);
        frame->link_mtu = link_mtu;
    }

    if (TUN_MTU_SIZE(frame) < TUN_MTU_MIN)
    {
        msg(M_WARN, "TUN MTU value (%d) must be at least %d", TUN_MTU_SIZE(frame), TUN_MTU_MIN);
        frame_print(frame, M_FATAL, "MTU is too small");
    }

    frame->link_mtu_dynamic = frame->link_mtu;
}

/*
 * Set the tun MTU dynamically.
 */
void
frame_set_mtu_dynamic(struct frame *frame, int mtu, unsigned int flags)
{

#ifdef ENABLE_DEBUG
    const int orig_mtu = mtu;
    const int orig_link_mtu_dynamic = frame->link_mtu_dynamic;
#endif

    ASSERT(mtu >= 0);

    if (flags & SET_MTU_TUN)
    {
        mtu += TUN_LINK_DELTA(frame);
    }

    if (!(flags & SET_MTU_UPPER_BOUND) || mtu < frame->link_mtu_dynamic)
    {
        frame->link_mtu_dynamic = constrain_int(
            mtu,
            EXPANDED_SIZE_MIN(frame),
            EXPANDED_SIZE(frame));
    }

    dmsg(D_MTU_DEBUG, "MTU DYNAMIC mtu=%d, flags=%u, %d -> %d",
         orig_mtu,
         flags,
         orig_link_mtu_dynamic,
         frame->link_mtu_dynamic);
}

/*
 * Move extra_frame octets into extra_tun.  Used by fragmenting code
 * to adjust frame relative to its position in the buffer processing
 * queue.
 */
void
frame_subtract_extra(struct frame *frame, const struct frame *src)
{
    frame->extra_frame -= src->extra_frame;
    frame->extra_tun   += src->extra_frame;
}

void
frame_print(const struct frame *frame,
            int level,
            const char *prefix)
{
    struct gc_arena gc = gc_new();
    struct buffer out = alloc_buf_gc(256, &gc);
    if (prefix)
    {
        buf_printf(&out, "%s ", prefix);
    }
    buf_printf(&out, "[");
    buf_printf(&out, " L:%d", frame->link_mtu);
    buf_printf(&out, " D:%d", frame->link_mtu_dynamic);
    buf_printf(&out, " EF:%d", frame->extra_frame);
    buf_printf(&out, " EB:%d", frame->extra_buffer);
    buf_printf(&out, " ET:%d", frame->extra_tun);
    buf_printf(&out, " EL:%d", frame->extra_link);
    buf_printf(&out, " ]");

    msg(level, "%s", out.data);
    gc_free(&gc);
}

#define MTUDISC_NOT_SUPPORTED_MSG "--mtu-disc is not supported on this OS"

void
set_mtu_discover_type(socket_descriptor_t sd, int mtu_type, sa_family_t proto_af)
{
    if (mtu_type >= 0)
    {
        switch (proto_af)
        {
#if defined(IP_MTU_DISCOVER)
            case AF_INET:
                if (setsockopt(sd, IPPROTO_IP, IP_MTU_DISCOVER,
                               (void *) &mtu_type, sizeof(mtu_type)))
                {
                    msg(M_ERR, "Error setting IP_MTU_DISCOVER type=%d on TCP/UDP socket",
                        mtu_type);
                }
                break;

#endif
#if defined(IPV6_MTU_DISCOVER)
            case AF_INET6:
                if (setsockopt(sd, IPPROTO_IPV6, IPV6_MTU_DISCOVER,
                               (void *) &mtu_type, sizeof(mtu_type)))
                {
                    msg(M_ERR, "Error setting IPV6_MTU_DISCOVER type=%d on TCP6/UDP6 socket",
                        mtu_type);
                }
                break;

#endif
            default:
                msg(M_FATAL, MTUDISC_NOT_SUPPORTED_MSG);
                break;
        }
    }
}

int
translate_mtu_discover_type_name(const char *name)
{
#if defined(IP_PMTUDISC_DONT) && defined(IP_PMTUDISC_WANT) && defined(IP_PMTUDISC_DO)
    if (!strcmp(name, "yes"))
    {
        return IP_PMTUDISC_DO;
    }
    if (!strcmp(name, "maybe"))
    {
        return IP_PMTUDISC_WANT;
    }
    if (!strcmp(name, "no"))
    {
        return IP_PMTUDISC_DONT;
    }
    msg(M_FATAL,
        "invalid --mtu-disc type: '%s' -- valid types are 'yes', 'maybe', or 'no'",
        name);
#else  /* if defined(IP_PMTUDISC_DONT) && defined(IP_PMTUDISC_WANT) && defined(IP_PMTUDISC_DO) */
    msg(M_FATAL, MTUDISC_NOT_SUPPORTED_MSG);
#endif
    return -1;                  /* NOTREACHED */
}

#if EXTENDED_SOCKET_ERROR_CAPABILITY

struct probehdr
{
    uint32_t ttl;
    struct timeval tv;
};

const char *
format_extended_socket_error(int fd, int *mtu, struct gc_arena *gc)
{
    int res;
    struct probehdr rcvbuf;
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct sock_extended_err *e;
    struct sockaddr_in addr;
    struct buffer out = alloc_buf_gc(256, gc);
    char *cbuf = (char *) gc_malloc(256, false, gc);

    *mtu = 0;

    while (true)
    {
        memset(&rcvbuf, -1, sizeof(rcvbuf));
        iov.iov_base = &rcvbuf;
        iov.iov_len = sizeof(rcvbuf);
        msg.msg_name = (uint8_t *) &addr;
        msg.msg_namelen = sizeof(addr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_flags = 0;
        msg.msg_control = cbuf;
        msg.msg_controllen = 256; /* size of cbuf */

        res = recvmsg(fd, &msg, MSG_ERRQUEUE);
        if (res < 0)
        {
            goto exit;
        }

        e = NULL;

        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            if (cmsg->cmsg_level == SOL_IP)
            {
                if (cmsg->cmsg_type == IP_RECVERR)
                {
                    e = (struct sock_extended_err *) CMSG_DATA(cmsg);
                }
                else
                {
                    buf_printf(&out,"CMSG=%d|", cmsg->cmsg_type);
                }
            }
        }
        if (e == NULL)
        {
            buf_printf(&out, "NO-INFO|");
            goto exit;
        }

        switch (e->ee_errno)
        {
            case ETIMEDOUT:
                buf_printf(&out, "ETIMEDOUT|");
                break;

            case EMSGSIZE:
                buf_printf(&out, "EMSGSIZE Path-MTU=%d|", e->ee_info);
                *mtu = e->ee_info;
                break;

            case ECONNREFUSED:
                buf_printf(&out, "ECONNREFUSED|");
                break;

            case EPROTO:
                buf_printf(&out, "EPROTO|");
                break;

            case EHOSTUNREACH:
                buf_printf(&out, "EHOSTUNREACH|");
                break;

            case ENETUNREACH:
                buf_printf(&out, "ENETUNREACH|");
                break;

            case EACCES:
                buf_printf(&out, "EACCES|");
                break;

            default:
                buf_printf(&out, "UNKNOWN|");
                break;
        }
    }

exit:
    buf_rmtail(&out, '|');
    return BSTR(&out);
}

void
set_sock_extended_error_passing(int sd)
{
    int on = 1;
    if (setsockopt(sd, SOL_IP, IP_RECVERR, (void *) &on, sizeof(on)))
    {
        msg(M_WARN | M_ERRNO,
            "Note: enable extended error passing on TCP/UDP socket failed (IP_RECVERR)");
    }
}

#endif /* if EXTENDED_SOCKET_ERROR_CAPABILITY */
