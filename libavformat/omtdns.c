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

#include <stdatomic.h>
#include <string.h>

#include "config_components.h"

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/common.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "network.h"
#include "omtdns.h"

#if CONFIG_LIBDNS_SD
#include <dns_sd.h>

#define OMT_DNS_SD_TYPE "_omt._tcp"
#define OMT_DNS_SD_WAIT (100 * 1000)

struct OMTDNSService {
    DNSServiceRef ref;
    pthread_t thread;
    atomic_int exiting;
    void *log_ctx;
};

typedef struct OMTDNSResolve {
    const char *name;
    DNSServiceRef ref;
    char host[256];
    int port;
    DNSServiceErrorType error;
    int done;
} OMTDNSResolve;

static void omt_dns_resolve_reply(DNSServiceRef ref, DNSServiceFlags flags,
                                  uint32_t interface_index,
                                  DNSServiceErrorType error, const char *fullname,
                                  const char *hosttarget, uint16_t port,
                                  uint16_t txt_len, const unsigned char *txt,
                                  void *context);

static void omt_dns_register_reply(DNSServiceRef ref, DNSServiceFlags flags,
                                   DNSServiceErrorType error, const char *name,
                                   const char *regtype, const char *domain,
                                   void *context)
{
    OMTDNSService *service = context;

    if (error != kDNSServiceErr_NoError)
        av_log(service->log_ctx, AV_LOG_WARNING,
               "OMT DNS-SD registration failed: %d\n", error);
}

static void *omt_dns_service_thread(void *arg)
{
    OMTDNSService *service = arg;

    while (!atomic_load(&service->exiting)) {
        int fd = DNSServiceRefSockFD(service->ref);
        int ret;

        if (fd < 0)
            break;
        ret = ff_network_wait_fd_timeout(fd, 0, OMT_DNS_SD_WAIT, NULL);
        if (ret == AVERROR(ETIMEDOUT))
            continue;
        if (ret < 0 || DNSServiceProcessResult(service->ref) != kDNSServiceErr_NoError)
            break;
    }
    return NULL;
}

int ff_omt_dns_sd_register(OMTDNSService **service, const char *name, int port,
                           void *log_ctx)
{
    OMTDNSService *s;
    DNSServiceErrorType error;
    int ret;

    if (!service || *service || !name || !name[0] || port < 1 || port > 65535)
        return AVERROR(EINVAL);
    s = av_mallocz(sizeof(*s));
    if (!s)
        return AVERROR(ENOMEM);
    s->log_ctx = log_ctx;
    atomic_init(&s->exiting, 0);
    error = DNSServiceRegister(&s->ref, 0, kDNSServiceInterfaceIndexAny,
                               name, OMT_DNS_SD_TYPE, NULL, NULL,
                               htons(port), 0, NULL,
                               omt_dns_register_reply, s);
    if (error != kDNSServiceErr_NoError) {
        av_log(log_ctx, AV_LOG_ERROR, "Could not register OMT DNS-SD source: %d\n", error);
        ret = AVERROR(EIO);
        goto fail;
    }
    ret = pthread_create(&s->thread, NULL, omt_dns_service_thread, s);
    if (ret) {
        ret = AVERROR(ret);
        goto fail;
    }
    *service = s;
    return 0;

fail:
    if (s->ref)
        DNSServiceRefDeallocate(s->ref);
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
    DNSServiceRefDeallocate(s->ref);
    av_free(s);
}

static void omt_dns_browse_reply(DNSServiceRef ref, DNSServiceFlags flags,
                                 uint32_t interface_index,
                                 DNSServiceErrorType error, const char *name,
                                 const char *regtype, const char *domain,
                                 void *context)
{
    OMTDNSResolve *result = context;

    if (error != kDNSServiceErr_NoError) {
        result->error = error;
        result->done = 1;
        return;
    }
    if (!(flags & kDNSServiceFlagsAdd) || strcmp(name, result->name) || result->ref)
        return;
    result->error = DNSServiceResolve(&result->ref, 0, interface_index,
                                      name, regtype, domain,
                                      omt_dns_resolve_reply, result);
    if (result->error != kDNSServiceErr_NoError)
        result->done = 1;
}

static void omt_dns_resolve_reply(DNSServiceRef ref, DNSServiceFlags flags,
                                  uint32_t interface_index,
                                  DNSServiceErrorType error, const char *fullname,
                                  const char *hosttarget, uint16_t port,
                                  uint16_t txt_len, const unsigned char *txt,
                                  void *context)
{
    OMTDNSResolve *result = context;
    struct addrinfo hints = { 0 }, *addresses = NULL;

    if (error != kDNSServiceErr_NoError) {
        result->error = error;
        result->done = 1;
        return;
    }
    if (!hosttarget || !hosttarget[0]) {
        result->error = kDNSServiceErr_NoSuchRecord;
        result->done = 1;
        return;
    }
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    result->error = getaddrinfo(hosttarget, NULL, &hints, &addresses);
    if (result->error != 0 || !addresses) {
        result->error = kDNSServiceErr_NoSuchRecord;
        result->done = 1;
        return;
    }
    result->error = kDNSServiceErr_NoSuchRecord;
    for (int family = 0; family < 2 && result->error != kDNSServiceErr_NoError; family++) {
        int wanted_family = family ? AF_INET6 : AF_INET;
        for (struct addrinfo *address = addresses; address; address = address->ai_next) {
            if (address->ai_family != wanted_family)
                continue;
            if (wanted_family == AF_INET6 &&
                IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)address->ai_addr)->sin6_addr))
                continue;
            if (!getnameinfo(address->ai_addr, address->ai_addrlen,
                             result->host, sizeof(result->host), NULL, 0,
                             NI_NUMERICHOST)) {
                result->error = kDNSServiceErr_NoError;
                result->port = ntohs(port);
                break;
            }
        }
    }
    freeaddrinfo(addresses);
    result->done = 1;
}

int ff_omt_dns_sd_resolve(const char *name, char *host, size_t host_size,
                          int *port, AVIOInterruptCB *interrupt_callback,
                          int64_t timeout, void *log_ctx)
{
    OMTDNSResolve result = { .name = name };
    DNSServiceRef browse = NULL;
    int64_t start_time = av_gettime_relative();
    DNSServiceErrorType error;
    int ret = 0;

    if (!name || !name[0] || !host || !host_size || !port || timeout <= 0)
        return AVERROR(EINVAL);
    error = DNSServiceBrowse(&browse, 0, kDNSServiceInterfaceIndexAny,
                             OMT_DNS_SD_TYPE, NULL, omt_dns_browse_reply, &result);
    if (error != kDNSServiceErr_NoError)
        return AVERROR(EIO);

    while (!result.done) {
        DNSServiceRef refs[2] = { browse, result.ref };
        for (int i = 0; i < FF_ARRAY_ELEMS(refs) && !result.done; i++) {
            int64_t remaining = timeout - (av_gettime_relative() - start_time);
            int fd;

            if (remaining <= 0) {
                ret = AVERROR(ETIMEDOUT);
                goto end;
            }
            if (!refs[i])
                continue;
            fd = DNSServiceRefSockFD(refs[i]);
            if (fd < 0) {
                ret = AVERROR(EIO);
                goto end;
            }
            ret = ff_network_wait_fd_timeout(fd, 0,
                                             FFMIN(remaining, OMT_DNS_SD_WAIT),
                                             interrupt_callback);
            if (ret == AVERROR(ETIMEDOUT))
                continue;
            if (ret < 0)
                goto end;
            DNSServiceErrorType process_error = DNSServiceProcessResult(refs[i]);
            if (process_error != kDNSServiceErr_NoError) {
                result.error = process_error;
                result.done = 1;
            }
        }
    }

    if (result.error != kDNSServiceErr_NoError) {
        ret = AVERROR(EIO);
        goto end;
    }
    if (av_strlcpy(host, result.host, host_size) >= host_size) {
        ret = AVERROR(ENOSPC);
        goto end;
    }
    *port = result.port;

end:
    if (result.ref)
        DNSServiceRefDeallocate(result.ref);
    if (browse)
        DNSServiceRefDeallocate(browse);
    if (ret < 0)
        av_log(log_ctx, AV_LOG_ERROR, "Could not resolve OMT DNS-SD source '%s'\n", name);
    return ret;
}
#endif