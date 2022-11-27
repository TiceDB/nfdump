/*
 *  Copyright (c) 2009-2022, Peter Haag
 *  Copyright (c) 2008, SWITCH - Teleinformatikdienste fuer Lehre und Forschung
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *   * Neither the name of the author nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "collector.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "bookkeeper.h"
#include "nfconf.h"
#include "nfdump.h"
#include "nffile.h"
#include "nfxV3.h"
#include "util.h"

/* local variables */
static uint32_t exporter_sysid = 0;
static char *DynamicSourcesDir = NULL;

/* local prototypes */
static uint32_t AssignExporterID(void);

#include "nffile_inline.c"

/* local functions */
static uint32_t AssignExporterID(void) {
    if (exporter_sysid >= 0xFFFF) {
        LogError("Too many exporters (id > 65535). Flow records collected but without reference to exporter");
        return 0;
    }

    return ++exporter_sysid;

}  // End of AssignExporterID

/* global functions */

int SetDynamicSourcesDir(FlowSource_t **FlowSource, char *dir) {
    if (*FlowSource) {
        LogError("Can not mix IP specific and any IP sources");
        return 0;
    }

    DynamicSourcesDir = dir;
    return 1;

}  // End of SetDynamicSourcesDir

int AddFlowSourceConfig(FlowSource_t **FlowSource) {
    char *ident, *ip, *flowdir;
    do {
        int ret = ConfGetExporter(&ident, &ip, &flowdir);
        if (ret > 0) {
            ret = AddFlowSource(FlowSource, ident, ip, flowdir);
            free(ident);
            free(ip);
            free(flowdir);
            if (ret == 0) return 0;
        } else {
            return 1;
        }
    } while (1);

    // unreached
}  // end of AddFlowSourceConfig

int AddFlowSource(FlowSource_t **FlowSource, char *ident, char *ip, char *flowpath) {
    FlowSource_t **source = NULL;
    int has_any_source = 0;
    int num_sources = 0;

    if (DynamicSourcesDir) {
        LogError("Can not mix IP specific and any IP sources");
        return 0;
    }

    source = FlowSource;
    while (*source) {
        has_any_source |= (*source)->any_source;
        source = &((*source)->next);
        num_sources++;
    }
    if ((ip && has_any_source) || (ip == NULL && num_sources > 0)) {
        LogError("Can not mix IP specific and any IP sources");
        return 0;
    }

    *source = (FlowSource_t *)calloc(1, sizeof(FlowSource_t));
    if (!*source) {
        LogError("malloc() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno));
        return 0;
    }

    if (ip == NULL) {
        (*source)->any_source = 1;
    } else {
        int ok = 0;
        if (strchr(ip, ':') != NULL) {
            // assume IPv6
            uint64_t _ip[2] = {0};
            ok = inet_pton(PF_INET6, ip, _ip);
            (*source)->sa_family = PF_INET6;
            (*source)->ip.V6[0] = ntohll(_ip[0]);
            (*source)->ip.V6[1] = ntohll(_ip[1]);
        } else {
            // IPv4
            uint32_t _ip = 0;
            ok = inet_pton(PF_INET, ip, &_ip);
            (*source)->sa_family = PF_INET;
            (*source)->ip.V4 = ntohl(_ip);
        }

        switch (ok) {
            case 0:
                LogError("Unparsable IP address: %s", ip);
                return 0;
            case 1:
                // success
                break;
            case -1:
                LogError("Error while parsing IP address: %s", strerror(errno));
                return 0;
                break;
        }
    }
    // fill in ident
    if (strlen(ident) >= IDENTLEN) {
        LogError("Source identifier too long: %s", ident);
        return 0;
    }
    if (strchr(ident, ' ')) {
        LogError("spaces not allowed in ident string: %s", ident);
        return 0;
    }
    strncpy((*source)->Ident, ident, IDENTLEN - 1);
    (*source)->Ident[IDENTLEN - 1] = '\0';

    // flowpath
    if (!CheckPath(flowpath, S_IFDIR)) {
        return 0;
    }

    char *path = realpath(flowpath, NULL);
    if (!path) {
        LogError("realpath() error %s: %s", flowpath, strerror(errno));
        return 0;
    }

    // remember path
    (*source)->datadir = path;

    char s[MAXPATHLEN];
    // cache current collector file
    if (snprintf(s, MAXPATHLEN - 1, "%s/%s.%lu", (*source)->datadir, NF_DUMPFILE, (unsigned long)getpid()) >= (MAXPATHLEN - 1)) {
        LogError("Path too long: %s", flowpath);
        return 0;
    }
    (*source)->current = strdup(s);
    if (!(*source)->current) {
        LogError("strdup() error: %s", strerror(errno));
        return 0;
    }

    LogVerbose("Add flow source: ident: %s, IP: %s, flowdir: %s", ident, ip ? ip : "any IP", flowpath);
    return 1;

}  // End of AddFlowSource

int AddFlowSourceString(FlowSource_t **FlowSource, char *argument) {
    char *ident = argument;
    char *ip = NULL;
    // separate IP address from ident
    if ((ip = strchr(ident, ',')) == NULL) {
        LogError("Syntax error for netflow source definition. Expect -n ident,IP,path");
        return 0;
    }
    *ip++ = '\0';

    char *flowpath = NULL;
    // separate path from IP
    if ((flowpath = strchr(ip, ',')) == NULL) {
        LogError("Syntax error for netflow source definition. Expect -n ident,IP,path");
        return 0;
    }
    *flowpath++ = '\0';

    return AddFlowSource(FlowSource, ident, ip, flowpath);

}  // End of AddFlowSourceString

FlowSource_t *AddDynamicSource(FlowSource_t **FlowSource, struct sockaddr_storage *ss) {
    FlowSource_t **source;
    void *ptr;
    char *s, ident[100], path[MAXPATHLEN];
    int err;

    union {
        struct sockaddr_storage *ss;
        struct sockaddr *sa;
        struct sockaddr_in *sa_in;
        struct sockaddr_in6 *sa_in6;
    } u;
    u.ss = ss;

    if (!DynamicSourcesDir) return NULL;

    source = FlowSource;
    while (*source) {
        source = &((*source)->next);
    }

    *source = (FlowSource_t *)calloc(1, sizeof(FlowSource_t));
    if (!*source) {
        LogError("malloc() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno));
        return NULL;
    }
    (*source)->next = NULL;
    (*source)->bookkeeper = NULL;
    (*source)->any_source = 0;
    (*source)->exporter_data = NULL;
    (*FlowSource)->exporter_count = 0;

    switch (ss->ss_family) {
        case PF_INET: {
#ifdef HAVE_STRUCT_SOCKADDR_STORAGE_SS_LEN
            if (ss->ss_len != sizeof(struct sockaddr_in)) {
                // malformed struct
                LogError("Malformed IPv4 socket struct in '%s', line '%d'", __FILE__, __LINE__);
                free(*source);
                *source = NULL;
                return NULL;
            }
#endif
            (*source)->sa_family = PF_INET;
            (*source)->ip.V6[0] = 0;
            (*source)->ip.V6[1] = 0;
            (*source)->ip.V4 = ntohl(u.sa_in->sin_addr.s_addr);
            ptr = &u.sa_in->sin_addr;
        } break;
        case PF_INET6: {
            uint64_t *ip_ptr = (uint64_t *)u.sa_in6->sin6_addr.s6_addr;

#ifdef HAVE_STRUCT_SOCKADDR_STORAGE_SS_LEN
            if (ss->ss_len != sizeof(struct sockaddr_in6)) {
                // malformed struct
                LogError("Malformed IPv6 socket struct in '%s', line '%d'", __FILE__, __LINE__);
                free(*source);
                *source = NULL;
                return NULL;
            }
#endif
            // ptr = &((struct sockaddr_in6 *)sa)->sin6_addr;
            (*source)->sa_family = PF_INET6;
            (*source)->ip.V6[0] = ntohll(ip_ptr[0]);
            (*source)->ip.V6[1] = ntohll(ip_ptr[1]);
            ptr = &u.sa_in6->sin6_addr;
        } break;
        default:
            // keep compiler happy
            (*source)->ip.V6[0] = 0;
            (*source)->ip.V6[1] = 0;
            ptr = NULL;

            LogError("Unknown sa fanily: %d in '%s', line '%d'", ss->ss_family, __FILE__, __LINE__);
            free(*source);
            *source = NULL;
            return NULL;
    }

    if (!ptr) {
        free(*source);
        *source = NULL;
        return NULL;
    }

    inet_ntop(ss->ss_family, ptr, ident, sizeof(ident));
    ident[99] = '\0';
    dbg_printf("Dynamic Flow Source IP: %s\n", ident);

    s = ident;
    while (*s != '\0') {
        if (*s == '.' || *s == ':') *s = '-';
        s++;
    }
    dbg_printf("Dynamic Flow Source ident: %s\n", ident);

    strncpy((*source)->Ident, ident, IDENTLEN - 1);
    (*source)->Ident[IDENTLEN - 1] = '\0';

    snprintf(path, MAXPATHLEN - 1, "%s/%s", DynamicSourcesDir, ident);
    path[MAXPATHLEN - 1] = '\0';

    err = mkdir(path, 0755);
    if (err != 0 && errno != EEXIST) {
        LogError("mkdir() error in %s line %d: %s", __FILE__, __LINE__, strerror(errno));
        free(*source);
        *source = NULL;
        return NULL;
    }
    (*source)->datadir = strdup(path);

    if (snprintf(path, MAXPATHLEN - 1, "%s/%s.%lu", (*source)->datadir, NF_DUMPFILE, (unsigned long)getpid()) >= (MAXPATHLEN - 1)) {
        LogError("Path too long: %s\n", path);
        free(*source);
        *source = NULL;
        return NULL;
    }
    (*source)->current = strdup(path);

    LogInfo("Dynamically add source ident: %s in directory: %s", ident, path);
    return *source;

}  // End of AddDynamicSource

int FlushInfoExporter(FlowSource_t *fs, exporter_info_record_t *exporter) {
    exporter->sysid = AssignExporterID();
    fs->exporter_count++;
    AppendToBuffer(fs->nffile, (void *)exporter, exporter->header.size);

#ifdef DEVEL
    {
#define IP_STRING_LEN 40
        char ipstr[IP_STRING_LEN];
        printf("Flush Exporter: ");
        if (exporter->sa_family == AF_INET) {
            uint32_t _ip = htonl(exporter->ip.V4);
            inet_ntop(AF_INET, &_ip, ipstr, sizeof(ipstr));
            printf("SysID: %u, IP: %16s, version: %u, ID: %2u\n", exporter->sysid, ipstr, exporter->version, exporter->id);
        } else if (exporter->sa_family == AF_INET6) {
            uint64_t _ip[2];
            _ip[0] = htonll(exporter->ip.V6[0]);
            _ip[1] = htonll(exporter->ip.V6[1]);
            inet_ntop(AF_INET6, &_ip, ipstr, sizeof(ipstr));
            printf("SysID: %u, IP: %40s, version: %u, ID: %2u\n", exporter->sysid, ipstr, exporter->version, exporter->id);
        } else {
            strncpy(ipstr, "<unknown>", IP_STRING_LEN);
            printf("**** Exporter IP version unknown ****\n");
        }
    }
#endif

    return 1;

}  // End of FlushInfoExporter

void FlushStdRecords(FlowSource_t *fs) {
    exporter_t *e = fs->exporter_data;

    while (e) {
        sampler_t *sampler = e->sampler;
        AppendToBuffer(fs->nffile, (void *)&(e->info), e->info.header.size);
        while (sampler) {
            AppendToBuffer(fs->nffile, (void *)&(sampler->record), sampler->record.size);
            sampler = sampler->next;
        }
        e = e->next;
    }

}  // End of FlushStdRecords

void FlushExporterStats(FlowSource_t *fs) {
    exporter_t *e = fs->exporter_data;
    exporter_stats_record_t *exporter_stats;
    uint32_t i, size;

    // idle collector ..
    if (!fs->exporter_count) return;

    size = sizeof(exporter_stats_record_t) + (fs->exporter_count - 1) * sizeof(struct exporter_stat_s);
    exporter_stats = (exporter_stats_record_t *)malloc(size);
    if (!exporter_stats) {
        LogError("malloc() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno));
        return;
    }
    exporter_stats->header.type = ExporterStatRecordType;
    exporter_stats->header.size = size;
    exporter_stats->stat_count = fs->exporter_count;

#ifdef DEVEL
    printf("Flush Exporter Stats: %u exporters, size: %u\n", fs->exporter_count, size);
#endif
    i = 0;
    while (e) {
        // prevent memory corruption - just in case .. should not happen anyway
        // continue loop for error reporting after while
        if (i >= fs->exporter_count) {
            i++;
            e = e->next;
            continue;
        }
        exporter_stats->stat[i].sysid = e->info.sysid;
        exporter_stats->stat[i].sequence_failure = e->sequence_failure;
        exporter_stats->stat[i].packets = e->packets;
        exporter_stats->stat[i].flows = e->flows;
#ifdef DEVEL
        printf("Stat: SysID: %u, version: %u, ID: %2u, Packets: %llu, Flows: %llu, Sequence Failures: %u, Padding Errors: %u\n", e->info.sysid,
               e->info.version, e->info.id, e->packets, e->flows, e->sequence_failure, e->padding_errors);

#endif
        // reset counters
        e->sequence_failure = 0;
        e->padding_errors = 0;
        e->packets = 0;
        e->flows = 0;

        i++;
        e = e->next;
    }
    AppendToBuffer(fs->nffile, (void *)exporter_stats, size);
    free(exporter_stats);

    if (i != fs->exporter_count) {
        LogError("ERROR: exporter stats: Expected %u records, but found %u in %s line %d: %s", fs->exporter_count, i, __FILE__, __LINE__,
                 strerror(errno));
    }

}  // End of FlushExporterStats
