/* $Id: PerformanceDarwin.cpp $ */
/** @file
 * VBox Darwin-specific Performance Classes implementation.
 */

/*
 * Copyright (C) 2008 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include <mach/mach_error.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/mach_time.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <iprt/err.h>
#include <iprt/log.h>
#include <iprt/param.h>
#include "Performance.h"

/* The following declarations are missing in 10.4.x SDK */
/* @todo Replace them with libproc.h and sys/proc_info.h when 10.4 is no longer supported */
extern "C" int proc_pidinfo(int pid, int flavor, uint64_t arg,  void *buffer, int buffersize);
struct proc_taskinfo {
    uint64_t    pti_virtual_size;       /* virtual memory size (bytes) */
    uint64_t    pti_resident_size;      /* resident memory size (bytes) */
    uint64_t    pti_total_user;         /* total time */
    uint64_t    pti_total_system;
    uint64_t    pti_threads_user;       /* existing threads only */
    uint64_t    pti_threads_system;
    int32_t     pti_policy;             /* default policy for new threads */
    int32_t     pti_faults;             /* number of page faults */
    int32_t     pti_pageins;            /* number of actual pageins */
    int32_t     pti_cow_faults;         /* number of copy-on-write faults */
    int32_t     pti_messages_sent;      /* number of messages sent */
    int32_t     pti_messages_received;  /* number of messages received */
    int32_t     pti_syscalls_mach;      /* number of mach system calls */
    int32_t     pti_syscalls_unix;      /* number of unix system calls */
    int32_t     pti_csw;                /* number of context switches */
    int32_t     pti_threadnum;          /* number of threads in the task */
    int32_t     pti_numrunning;         /* number of running threads */
    int32_t     pti_priority;           /* task priority*/
};
#define PROC_PIDTASKINFO 4

namespace pm {

class CollectorDarwin : public CollectorHAL
{
public:
    CollectorDarwin();
    virtual int getRawHostCpuLoad(uint64_t *user, uint64_t *kernel, uint64_t *idle);
    virtual int getHostMemoryUsage(ULONG *total, ULONG *used, ULONG *available);
    virtual int getRawProcessCpuLoad(RTPROCESS process, uint64_t *user, uint64_t *kernel, uint64_t *total);
    virtual int getProcessMemoryUsage(RTPROCESS process, ULONG *used);
private:
    ULONG totalRAM;
};

CollectorHAL *createHAL()
{
    return new CollectorDarwin();
}

CollectorDarwin::CollectorDarwin()
{
    uint64_t hostMemory;
    int mib[2];
    size_t size;

    mib[0] = CTL_HW;
    mib[1] = HW_MEMSIZE;

    size = sizeof(hostMemory);
    if (sysctl(mib, 2, &hostMemory, &size, NULL, 0) == -1) {
        Log(("sysctl() -> %s", strerror(errno)));
        hostMemory = 0;
    }
    totalRAM = (ULONG)(hostMemory / 1024);
}

int CollectorDarwin::getRawHostCpuLoad(uint64_t *user, uint64_t *kernel, uint64_t *idle)
{
    kern_return_t krc;
    mach_msg_type_number_t count;
    host_cpu_load_info_data_t info;

    count = HOST_CPU_LOAD_INFO_COUNT;

    krc = host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&info, &count);
    if (krc != KERN_SUCCESS)
    {
        Log(("host_statistics() -> %s", mach_error_string(krc)));
        return RTErrConvertFromDarwinKern(krc);
    }

    *user = (uint64_t)info.cpu_ticks[CPU_STATE_USER]
                    + info.cpu_ticks[CPU_STATE_NICE];
    *kernel = (uint64_t)info.cpu_ticks[CPU_STATE_SYSTEM];
    *idle = (uint64_t)info.cpu_ticks[CPU_STATE_IDLE];
    return VINF_SUCCESS;
}

int CollectorDarwin::getHostMemoryUsage(ULONG *total, ULONG *used, ULONG *available)
{
    kern_return_t krc;
    mach_msg_type_number_t count;
    vm_statistics_data_t info;

    count = HOST_VM_INFO_COUNT;

    krc = host_statistics(mach_host_self(), HOST_VM_INFO, (host_info_t)&info, &count);
    if (krc != KERN_SUCCESS)
    {
        Log(("host_statistics() -> %s", mach_error_string(krc)));
        return RTErrConvertFromDarwinKern(krc);
    }

    *total = totalRAM;
    *available = info.free_count * (PAGE_SIZE / 1024);
    *used = *total - *available;
    return VINF_SUCCESS;
}

static int getProcessInfo(RTPROCESS process, struct proc_taskinfo *tinfo)
{
    LogAleksey(("getProcessInfo() getting info for %d", process));
    int nb = proc_pidinfo(process, PROC_PIDTASKINFO, 0,  tinfo, sizeof(*tinfo));
    if (nb <= 0)
    {
        int rc = errno;
        Log(("proc_pidinfo() -> %s", strerror(rc)));
        return RTErrConvertFromDarwin(rc);
    }
    else if ((unsigned int)nb < sizeof(*tinfo))
    {
        Log(("proc_pidinfo() -> too few bytes %d", nb));
        return VERR_INTERNAL_ERROR;
    }
    return VINF_SUCCESS;
}

int CollectorDarwin::getRawProcessCpuLoad(RTPROCESS process, uint64_t *user, uint64_t *kernel, uint64_t *total)
{
    struct proc_taskinfo tinfo;

    int rc = getProcessInfo(process, &tinfo);
    if (RT_SUCCESS(rc))
    {
        *user = tinfo.pti_total_user;
        *kernel = tinfo.pti_total_system;
        *total = mach_absolute_time();
    }
    return rc;
}

int CollectorDarwin::getProcessMemoryUsage(RTPROCESS process, ULONG *used)
{
    struct proc_taskinfo tinfo;

    int rc = getProcessInfo(process, &tinfo);
    if (RT_SUCCESS(rc))
    {
        *used = tinfo.pti_resident_size / 1024;
    }
    return rc;
}

}

