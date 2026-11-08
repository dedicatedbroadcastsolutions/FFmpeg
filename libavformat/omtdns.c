/*
 * OMT DNS-SD helpers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#include <stdatomic.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "network.h"
#include "omtdns.h"

#define OMT_MDNS_GROUP "224.0.0.251"
#define OMT_MDNS_PORT 5353
#define OMT_MDNS_PACKET_SIZE 1500
#define OMT_MDNS_WAIT 10000
#define OMT_MDNS_ANNOUNCE (60 * AV_TIME_BASE)
#define OMT_DNS_TYPE_A 1
#define OMT_DNS_TYPE_PTR 12
#define OMT_DNS_TYPE_AAAA 28
#define OMT_DNS_TYPE_SRV 33
#define OMT_DNS_TYPE_ANY 255
#define OMT_DNS_CLASS_IN 1
#define OMT_DNS_CLASS_FLUSH 0x8001
#define OMT_DNS_LABELS 32

typedef struct OMTDNSName {
    uint8_t count;
    uint8_t length[OMT_DNS_LABELS];
    uint8_t label[OMT_DNS_LABELS][64];
} OMTDNSName;

typedef struct OMTDNSAddressRecord {
    OMTDNSName owner;
    uint16_t type;
    uint8_t data[16];
} OMTDNSAddressRecord;

typedef struct OMTDNSResolveState {
    const char *service_name;
    int have_instance;
    int have_target;
    uint16_t port;
    OMTDNSName target;
    char host[256];
    int have_address;
} OMTDNSResolveState;

struct OMTDNSService {
    int socket;
    pthread_t thread;
    atomic_int exiting;
    char service_name[64];
    char hostname[256];
    uint8_t address[4];
    uint16_t port;
    void *log_ctx;
};

static int dns_write_u16(uint8_t *buffer, size_t size, size_t *offset, uint16_t value)
{
    if (*offset > size - 2)
        return AVERROR(ENOSPC);
    AV_WB16(buffer + *offset, value);
    *offset += 2;
    return 0;
}

static int dns_write_label(uint8_t *buffer, size_t size, size_t *offset,
                           const uint8_t *label, size_t length)
{
    if (!length || length > 63 || *offset > size - length - 1)
        return AVERROR(ENOSPC);
    buffer[(*offset)++] = length;
    memcpy(buffer + *offset, label, length);
    *offset += length;
    return 0;
}

static int dns_write_service_type(uint8_t *buffer, size_t size, size_t *offset)
{
    int ret;

    if ((ret = dns_write_label(buffer, size, offset, (const uint8_t *)"_omt", 4)) < 0 ||
        (ret = dns_write_label(buffer, size, offset, (const uint8_t *)"_tcp", 4)) < 0 ||
        (ret = dns_write_label(buffer, size, offset, (const uint8_t *)"local", 5)) < 0)
        return ret;
    if (*offset >= size)
        return AVERROR(ENOSPC);
    buffer[(*offset)++] = 0;
    return 0;
}

static int dns_write_instance(const char *service_name, uint8_t *buffer,
                              size_t size, size_t *offset)
{
    int ret;

    if ((ret = dns_write_label(buffer, size, offset,
                               (const uint8_t *)service_name, strlen(service_name))) < 0 ||
        (ret = dns_write_label(buffer, size, offset, (const uint8_t *)"_omt", 4)) < 0 ||
        (ret = dns_write_label(buffer, size, offset, (const uint8_t *)"_tcp", 4)) < 0 ||
        (ret = dns_write_label(buffer, size, offset, (const uint8_t *)"local", 5)) < 0)
        return ret;
    if (*offset >= size)
        return AVERROR(ENOSPC);
    buffer[(*offset)++] = 0;
    return 0;
}

static int dns_write_hostname(const char *hostname, uint8_t *buffer,
                              size_t size, size_t *offset)
{
    const char *label = hostname;
    const char *dot;
    int ret;

    while ((dot = strchr(label, '.'))) {
        if (dot == label)
            return AVERROR(EINVAL);
        ret = dns_write_label(buffer, size, offset, (const uint8_t *)label, dot - label);
        if (ret < 0)
            return ret;
        label = dot + 1;
    }
    if (*label && strcmp(label, "local")) {
        ret = dns_write_label(buffer, size, offset, (const uint8_t *)label, strlen(label));
        if (ret < 0)
            return ret;
        ret = dns_write_label(buffer, size, offset, (const uint8_t *)"local", 5);
        if (ret < 0)
            return ret;
    } else if (!*label) {
        return AVERROR(EINVAL);
    }
    if (*offset >= size)
        return AVERROR(ENOSPC);
    buffer[(*offset)++] = 0;
    return 0;
}

static int dns_write_record_header(uint8_t *buffer, size_t size, size_t *offset,
                                   const char *owner, int owner_type,
                                   uint16_t type, uint16_t class, uint32_t ttl,
                                   size_t *rdlength_offset)
{
    int ret;

    if (owner_type == OMT_DNS_TYPE_PTR) {
        ret = dns_write_service_type(buffer, size, offset);
    } else if (owner_type == OMT_DNS_TYPE_SRV) {
        ret = dns_write_instance(owner, buffer, size, offset);
    } else {
        ret = dns_write_hostname(owner, buffer, size, offset);
    }
    if (ret < 0)
        return ret;
    if (*offset > size - 10)
        return AVERROR(ENOSPC);
    AV_WB16(buffer + *offset, type);
    AV_WB16(buffer + *offset + 2, class);
    AV_WB32(buffer + *offset + 4, ttl);
    *rdlength_offset = *offset + 8;
    AV_WB16(buffer + *rdlength_offset, 0);
    *offset += 10;
    return 0;
}

static int dns_write_record(uint8_t *buffer, size_t size, size_t *offset,
                            const OMTDNSService *service, int type,
                            uint32_t ttl)
{
    size_t rdlength_offset, data_offset;
    int ret;

    if (type == OMT_DNS_TYPE_PTR) {
        ret = dns_write_record_header(buffer, size, offset, NULL, type, type,
                                      OMT_DNS_CLASS_IN, ttl, &rdlength_offset);
        data_offset = *offset;
        if (ret >= 0)
            ret = dns_write_instance(service->service_name, buffer, size, offset);
    } else if (type == OMT_DNS_TYPE_SRV) {
        ret = dns_write_record_header(buffer, size, offset, service->service_name,
                                      type, type, OMT_DNS_CLASS_FLUSH, ttl,
                                      &rdlength_offset);
        data_offset = *offset;
        if (ret >= 0) {
            if (*offset > size - 6)
                ret = AVERROR(ENOSPC);
            else {
                AV_WB16(buffer + *offset, 0);
                AV_WB16(buffer + *offset + 2, 0);
                AV_WB16(buffer + *offset + 4, service->port);
                *offset += 6;
                ret = dns_write_hostname(service->hostname, buffer, size, offset);
            }
        }
    } else {
        ret = dns_write_record_header(buffer, size, offset, service->hostname,
                                      0, OMT_DNS_TYPE_A, OMT_DNS_CLASS_FLUSH,
                                      ttl, &rdlength_offset);
        data_offset = *offset;
        if (ret >= 0) {
            if (*offset > size - sizeof(service->address))
                ret = AVERROR(ENOSPC);
            else {
                memcpy(buffer + *offset, service->address, sizeof(service->address));
                *offset += sizeof(service->address);
            }
        }
    }
    if (ret >= 0)
        AV_WB16(buffer + rdlength_offset, *offset - data_offset);
    return ret;
}

static int dns_build_announcement(const OMTDNSService *service, uint8_t *buffer,
                                  size_t size, uint32_t ttl)
{
    size_t offset = 12;
    int ret;

    memset(buffer, 0, size);
    AV_WB16(buffer + 2, 0x8400);
    AV_WB16(buffer + 6, 3);
    if ((ret = dns_write_record(buffer, size, &offset, service,
                                OMT_DNS_TYPE_PTR, ttl)) < 0 ||
        (ret = dns_write_record(buffer, size, &offset, service,
                                OMT_DNS_TYPE_SRV, ttl)) < 0 ||
        (ret = dns_write_record(buffer, size, &offset, service,
                                OMT_DNS_TYPE_A, ttl)) < 0)
        return ret;
    return offset;
}

static int dns_name_read(const uint8_t *packet, size_t packet_size, size_t *offset,
                         OMTDNSName *name)
{
    size_t pos = *offset, resume = 0;
    int jumped = 0;

    memset(name, 0, sizeof(*name));
    for (int steps = 0; steps < 128; steps++) {
        uint8_t length;

        if (pos >= packet_size)
            return AVERROR_INVALIDDATA;
        length = packet[pos++];
        if ((length & 0xC0) == 0xC0) {
            size_t pointer;
            if (pos >= packet_size)
                return AVERROR_INVALIDDATA;
            pointer = ((size_t)(length & 0x3F) << 8) | packet[pos++];
            if (pointer >= packet_size)
                return AVERROR_INVALIDDATA;
            if (!jumped) {
                resume = pos;
                jumped = 1;
            }
            pos = pointer;
            continue;
        }
        if (length & 0xC0)
            return AVERROR_INVALIDDATA;
        if (!length) {
            *offset = jumped ? resume : pos;
            return 0;
        }
        if (length > 63 || name->count >= OMT_DNS_LABELS ||
            length > packet_size - pos)
            return AVERROR_INVALIDDATA;
        name->length[name->count] = length;
        memcpy(name->label[name->count], packet + pos, length);
        name->count++;
        pos += length;
    }
    return AVERROR_INVALIDDATA;
}

static int dns_label_equal(const OMTDNSName *name, int index, const char *label)
{
    size_t length = strlen(label);
    return index < name->count && name->length[index] == length &&
           !memcmp(name->label[index], label, length);
}

static int dns_name_is_service_type(const OMTDNSName *name)
{
    return name->count == 3 && dns_label_equal(name, 0, "_omt") &&
           dns_label_equal(name, 1, "_tcp") && dns_label_equal(name, 2, "local");
}

static int dns_name_is_instance(const OMTDNSName *name, const char *instance)
{
    size_t length = strlen(instance);
    return name->count == 4 && name->length[0] == length &&
           !memcmp(name->label[0], instance, length) &&
           dns_label_equal(name, 1, "_omt") && dns_label_equal(name, 2, "_tcp") &&
           dns_label_equal(name, 3, "local");
}

static int dns_name_equal(const OMTDNSName *a, const OMTDNSName *b)
{
    if (a->count != b->count)
        return 0;
    for (int i = 0; i < a->count; i++)
        if (a->length[i] != b->length[i] ||
            memcmp(a->label[i], b->label[i], a->length[i]))
            return 0;
    return 1;
}

static int dns_name_to_host(const OMTDNSName *name, char *host, size_t host_size)
{
    size_t offset = 0;

    for (int i = 0; i < name->count; i++) {
        size_t length = name->length[i];
        if (offset + length + (i != 0) >= host_size)
            return AVERROR(ENOSPC);
        if (i)
            host[offset++] = '.';
        memcpy(host + offset, name->label[i], length);
        offset += length;
    }
    if (offset >= host_size)
        return AVERROR(ENOSPC);
    host[offset] = 0;
    return 0;
}

static int dns_skip_question(const uint8_t *packet, size_t packet_size, size_t *offset)
{
    OMTDNSName name;
    int ret = dns_name_read(packet, packet_size, offset, &name);
    if (ret < 0 || *offset > packet_size || packet_size - *offset < 4)
        return AVERROR_INVALIDDATA;
    *offset += 4;
    return 0;
}

static int dns_query_matches(const uint8_t *packet, size_t packet_size,
                             const OMTDNSService *service)
{
    uint16_t questions;
    size_t offset = 12;

    if (packet_size < 12 || (AV_RB16(packet + 2) & 0x8000))
        return 0;
    questions = AV_RB16(packet + 4);
    for (int i = 0; i < questions; i++) {
        OMTDNSName name;
        uint16_t type;
        if (dns_name_read(packet, packet_size, &offset, &name) < 0 ||
            offset > packet_size || packet_size - offset < 4)
            return 0;
        type = AV_RB16(packet + offset);
        offset += 4;
        if ((dns_name_is_service_type(&name) &&
             (type == OMT_DNS_TYPE_PTR || type == OMT_DNS_TYPE_ANY)) ||
            (dns_name_is_instance(&name, service->service_name) &&
             (type == OMT_DNS_TYPE_SRV || type == OMT_DNS_TYPE_ANY)))
            return 1;
    }
    return 0;
}

static int dns_build_query(uint8_t *packet, size_t packet_size,
                           const OMTDNSResolveState *state)
{
    size_t offset = 12;
    int ret;
    uint16_t type;

    memset(packet, 0, packet_size);
    AV_WB16(packet + 4, 1);
    if (state->have_target) {
        ret = dns_write_hostname(state->host, packet, packet_size, &offset);
        type = OMT_DNS_TYPE_ANY;
    } else if (state->have_instance) {
        ret = dns_write_instance(state->service_name, packet, packet_size, &offset);
        type = OMT_DNS_TYPE_SRV;
    } else {
        ret = dns_write_service_type(packet, packet_size, &offset);
        type = OMT_DNS_TYPE_PTR;
    }
    if (ret < 0 || dns_write_u16(packet, packet_size, &offset, type) < 0 ||
        dns_write_u16(packet, packet_size, &offset, OMT_DNS_CLASS_IN) < 0)
        return AVERROR(ENOSPC);
    return offset;
}

static int dns_parse_response(const uint8_t *packet, size_t packet_size,
                              OMTDNSResolveState *state)
{
    OMTDNSAddressRecord addresses[16];
    int address_count = 0;
    uint16_t questions;
    unsigned int records;
    size_t offset = 12;

    if (packet_size < 12 || !(AV_RB16(packet + 2) & 0x8000))
        return 0;
    questions = AV_RB16(packet + 4);
    records = AV_RB16(packet + 6) + AV_RB16(packet + 8) + AV_RB16(packet + 10);
    for (int i = 0; i < questions; i++)
        if (dns_skip_question(packet, packet_size, &offset) < 0)
            return AVERROR_INVALIDDATA;

    for (int i = 0; i < records; i++) {
        OMTDNSName owner;
        uint16_t type, class, data_size;
        size_t data_offset, end_offset;
        if (dns_name_read(packet, packet_size, &offset, &owner) < 0 ||
            offset > packet_size || packet_size - offset < 10)
            return AVERROR_INVALIDDATA;
        type = AV_RB16(packet + offset);
        class = AV_RB16(packet + offset + 2) & 0x7FFF;
        data_size = AV_RB16(packet + offset + 8);
        offset += 10;
        data_offset = offset;
        end_offset = data_offset + data_size;
        if (end_offset > packet_size)
            return AVERROR_INVALIDDATA;
        if (class != OMT_DNS_CLASS_IN)
            goto next_record;

        if (type == OMT_DNS_TYPE_PTR && dns_name_is_service_type(&owner)) {
            OMTDNSName instance;
            size_t name_offset = data_offset;
            if (dns_name_read(packet, packet_size, &name_offset, &instance) >= 0 &&
                dns_name_is_instance(&instance, state->service_name))
                state->have_instance = 1;
        } else if (type == OMT_DNS_TYPE_SRV &&
                   dns_name_is_instance(&owner, state->service_name) && data_size >= 7) {
            OMTDNSName target;
            size_t name_offset = data_offset + 6;
            if (dns_name_read(packet, packet_size, &name_offset, &target) >= 0 &&
                dns_name_to_host(&target, state->host, sizeof(state->host)) >= 0) {
                state->port = AV_RB16(packet + data_offset + 4);
                state->target = target;
                state->have_instance = state->have_target = 1;
            }
        } else if ((type == OMT_DNS_TYPE_A && data_size == 4) ||
                   (type == OMT_DNS_TYPE_AAAA && data_size == 16)) {
            if (address_count < FF_ARRAY_ELEMS(addresses)) {
                addresses[address_count].owner = owner;
                addresses[address_count].type = type;
                memcpy(addresses[address_count].data, packet + data_offset, data_size);
                address_count++;
            }
        }
next_record:
        offset = end_offset;
    }

    if (state->have_target) {
        for (int i = 0; i < address_count; i++) {
            if (!dns_name_equal(&addresses[i].owner, &state->target))
                continue;
            if (addresses[i].type == OMT_DNS_TYPE_A) {
                if (!inet_ntop(AF_INET, addresses[i].data, state->host, sizeof(state->host)))
                    return AVERROR_INVALIDDATA;
                state->have_address = 1;
                break;
            }
            if (addresses[i].type == OMT_DNS_TYPE_AAAA) {
                struct in6_addr address;
                memcpy(&address, addresses[i].data, sizeof(address));
                if (IN6_IS_ADDR_LINKLOCAL(&address))
                    continue;
                if (!inet_ntop(AF_INET6, &address, state->host, sizeof(state->host)))
                    return AVERROR_INVALIDDATA;
                state->have_address = 1;
            }
        }
    }
    return 0;
}

static int dns_get_local_ipv4(uint8_t address[4])
{
    struct sockaddr_in local = { .sin_family = AF_INET };
    struct sockaddr_in remote = { .sin_family = AF_INET,
                                  .sin_port = htons(OMT_MDNS_PORT) };
    socklen_t address_size = sizeof(local);
    int fd, ret;

    if (inet_pton(AF_INET, OMT_MDNS_GROUP, &remote.sin_addr) != 1)
        return AVERROR(EINVAL);
    fd = ff_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL);
    if (fd < 0)
        return ff_neterrno();
    ret = connect(fd, (struct sockaddr *)&remote, sizeof(remote));
    if (ret < 0 || getsockname(fd, (struct sockaddr *)&local, &address_size) < 0) {
        ret = ff_neterrno();
        closesocket(fd);
        return ret;
    }
    memcpy(address, &local.sin_addr, 4);
    closesocket(fd);
    return 0;
}

static int dns_open_socket(int *socket_fd, const uint8_t address[4], void *log_ctx)
{
    struct sockaddr_in local = { .sin_family = AF_INET,
                                 .sin_port = htons(OMT_MDNS_PORT) };
    struct ip_mreq membership = { 0 };
    unsigned char ttl = 255, loop = 1;
    int reuse = 1;
    int fd, ret;

    local.sin_addr.s_addr = htonl(INADDR_ANY);
    membership.imr_multiaddr.s_addr = inet_addr(OMT_MDNS_GROUP);
    memcpy(&membership.imr_interface, address, 4);
    fd = ff_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, log_ctx);
    if (fd < 0)
        return ff_neterrno();
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&reuse, sizeof(reuse)) < 0 ||
        bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)&membership,
                   sizeof(membership)) < 0 ||
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                   (const void *)&membership.imr_interface,
                   sizeof(membership.imr_interface)) < 0 ||
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const void *)&ttl, sizeof(ttl)) < 0 ||
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const void *)&loop, sizeof(loop)) < 0 ||
        ff_socket_nonblock(fd, 1) < 0) {
        ret = ff_neterrno();
        closesocket(fd);
        return ret;
    }
    *socket_fd = fd;
    return 0;
}

static int dns_send_announcement(OMTDNSService *service, uint32_t ttl)
{
    uint8_t packet[OMT_MDNS_PACKET_SIZE];
    struct sockaddr_in destination = { .sin_family = AF_INET,
                                       .sin_port = htons(OMT_MDNS_PORT) };
    int size = dns_build_announcement(service, packet, sizeof(packet), ttl);
    int ret;

    if (size < 0)
        return size;
    destination.sin_addr.s_addr = inet_addr(OMT_MDNS_GROUP);
    ret = sendto(service->socket, packet, size, 0,
                 (struct sockaddr *)&destination, sizeof(destination));
    if (ret < 0)
        return ff_neterrno();
    return ret == size ? 0 : (ret < 0 ? ret : AVERROR(EIO));
}

static void *dns_service_thread(void *arg)
{
    OMTDNSService *service = arg;
    uint8_t packet[OMT_MDNS_PACKET_SIZE];
    int64_t next_announce = 0;

    while (!atomic_load(&service->exiting)) {
        int64_t now = av_gettime_relative();
        int ret;

        if (now >= next_announce) {
            dns_send_announcement(service, 120);
            next_announce = now + OMT_MDNS_ANNOUNCE;
        }
        ret = ff_network_wait_fd_timeout(service->socket, 0, OMT_MDNS_WAIT, NULL);
        if (ret == AVERROR(ETIMEDOUT))
            continue;
        if (ret < 0)
            break;
        ret = recvfrom(service->socket, packet, sizeof(packet), 0, NULL, NULL);
        if (ret < 0) {
            ret = ff_neterrno();
            if (ret == AVERROR(EAGAIN) || ret == AVERROR(EINTR))
                continue;
            break;
        }
        if (dns_query_matches(packet, ret, service))
            dns_send_announcement(service, 120);
    }
    dns_send_announcement(service, 0);
    return NULL;
}

int ff_omt_dns_sd_register(OMTDNSService **service, const char *name, int port,
                           void *log_ctx)
{
    OMTDNSService *s;
    int ret;

    if (!service || *service || !name || !name[0] || strlen(name) > 63 ||
        port < 1 || port > 65535)
        return AVERROR(EINVAL);
    s = av_mallocz(sizeof(*s));
    if (!s)
        return AVERROR(ENOMEM);
    s->socket = -1;
    s->port = port;
    s->log_ctx = log_ctx;
    av_strlcpy(s->service_name, name, sizeof(s->service_name));
    if (gethostname(s->hostname, sizeof(s->hostname) - 1) < 0) {
        ret = AVERROR(EIO);
        goto fail;
    }
    s->hostname[sizeof(s->hostname) - 1] = 0;
    if ((ret = dns_get_local_ipv4(s->address)) < 0)
        goto fail;
    if ((ret = dns_open_socket(&s->socket, s->address, log_ctx)) < 0)
        goto fail;
    atomic_init(&s->exiting, 0);
    ret = pthread_create(&s->thread, NULL, dns_service_thread, s);
    if (ret) {
        ret = AVERROR(ret);
        goto fail;
    }
    *service = s;
    return 0;

fail:
    av_log(log_ctx, AV_LOG_ERROR, "Could not register OMT DNS-SD source: %s\n",
           av_err2str(ret));
    if (s->socket >= 0)
        closesocket(s->socket);
    av_free(s);
    return ret;
}

void ff_omt_dns_sd_unregister(OMTDNSService **service)
{
    OMTDNSService *s;

    if (!service || !*service)
        return;
    s = *service;
    *service = NULL;
    atomic_store(&s->exiting, 1);
    pthread_join(s->thread, NULL);
    closesocket(s->socket);
    av_free(s);
}

int ff_omt_dns_sd_resolve(const char *name, char *host, size_t host_size,
                          int *port, AVIOInterruptCB *interrupt_callback,
                          int64_t timeout, void *log_ctx)
{
    OMTDNSResolveState state = { .service_name = name };
    uint8_t packet[OMT_MDNS_PACKET_SIZE];
    uint8_t localaddr[4];
    struct sockaddr_in destination = { .sin_family = AF_INET,
                                       .sin_port = htons(OMT_MDNS_PORT) };
    int socket_fd = -1;
    int64_t start_time, next_query = 0;
    int ret;

    if (!name || !name[0] || strlen(name) > 63 || !host || !host_size ||
        !port || timeout <= 0)
        return AVERROR(EINVAL);
    if ((ret = dns_get_local_ipv4(localaddr)) < 0 ||
        (ret = dns_open_socket(&socket_fd, localaddr, log_ctx)) < 0)
        return ret;
    destination.sin_addr.s_addr = inet_addr(OMT_MDNS_GROUP);
    start_time = av_gettime_relative();

    while (!state.have_address) {
        int64_t now = av_gettime_relative();
        int64_t remaining = timeout - (now - start_time);

        if (remaining <= 0) {
            ret = AVERROR(ETIMEDOUT);
            goto end;
        }
        if (now >= next_query) {
            int query_size = dns_build_query(packet, sizeof(packet), &state);
            if (query_size < 0) {
                ret = query_size;
                goto end;
            }
            ret = sendto(socket_fd, packet, query_size, 0,
                         (struct sockaddr *)&destination, sizeof(destination));
            if (ret < 0) {
                ret = ff_neterrno();
                goto end;
            }
            next_query = now + AV_TIME_BASE;
        }
        ret = ff_network_wait_fd_timeout(socket_fd, 0,
                                         FFMIN(remaining, OMT_MDNS_WAIT),
                                         interrupt_callback);
        if (ret == AVERROR(ETIMEDOUT))
            continue;
        if (ret < 0)
            goto end;
        ret = recvfrom(socket_fd, packet, sizeof(packet), 0, NULL, NULL);
        if (ret < 0) {
            ret = ff_neterrno();
            if (ret == AVERROR(EAGAIN) || ret == AVERROR(EINTR))
                continue;
            goto end;
        }
        ret = dns_parse_response(packet, ret, &state);
        if (ret < 0)
            goto end;
    }
    if (av_strlcpy(host, state.host, host_size) >= host_size) {
        ret = AVERROR(ENOSPC);
        goto end;
    }
    *port = state.port;
    ret = 0;

end:
    if (socket_fd >= 0)
        closesocket(socket_fd);
    if (ret < 0)
        av_log(log_ctx, AV_LOG_ERROR, "Could not resolve OMT DNS-SD source '%s'\n", name);
    return ret;
}

int ff_omt_dns_sd_test(void)
{
    static const char name[] = "Test Host (Camera One)";
    OMTDNSService service = {
        .service_name = "Test Host (Camera One)",
        .hostname = "omt-test-host",
        .address = { 192, 0, 2, 10 },
        .port = 1234,
    };
    OMTDNSResolveState state = { .service_name = name };
    uint8_t packet[OMT_MDNS_PACKET_SIZE];
    int size;

    size = dns_build_query(packet, sizeof(packet), &state);
    if (size < 0 || !dns_query_matches(packet, size, &service))
        return AVERROR_INVALIDDATA;
    size = dns_build_announcement(&service, packet, sizeof(packet), 120);
    if (size < 0 || dns_parse_response(packet, size, &state) < 0 ||
        !state.have_instance || !state.have_target || !state.have_address ||
        state.port != service.port || strcmp(state.host, "192.0.2.10"))
        return AVERROR_INVALIDDATA;
    return 0;
}