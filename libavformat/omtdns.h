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

#ifndef AVFORMAT_OMTDNS_H
#define AVFORMAT_OMTDNS_H

#include <stddef.h>

#include "avformat.h"

typedef struct OMTDNSService OMTDNSService;

int ff_omt_dns_sd_register(OMTDNSService **service, const char *name, int port,
                           void *log_ctx);
void ff_omt_dns_sd_unregister(OMTDNSService **service);
int ff_omt_dns_sd_resolve(const char *name, char *host, size_t host_size,
                          int *port, AVIOInterruptCB *interrupt_callback,
                          int64_t timeout, void *log_ctx);
int ff_omt_dns_sd_test(void);

#endif