/******************************************************************************
 * kernel.c
 * 
 * Assorted crap goes here, including the initial C entry point, jumped at
 * from head.S.
 * 
 * Copyright (c) 2002-2003, K A Fraser & R Neugebauer
 * Copyright (c) 2005, Grzegorz Milos, Intel Research Cambridge
 * Copyright (c) 2006, Robert Kaiser, FH Wiesbaden
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 */

#include <mini-os/os.h>
#include <mini-os/kernel.h>
#include <mini-os/hypervisor.h>
#include <mini-os/mm.h>
#include <mini-os/events.h>
#include <mini-os/time.h>
#include <mini-os/types.h>
#include <mini-os/lib.h>
#include <mini-os/sched.h>
#include <mini-os/xenbus.h>
#include <mini-os/gnttab.h>
#include <mini-os/netfront.h>
#include <mini-os/blkfront.h>
#include <mini-os/fbfront.h>
#include <mini-os/pcifront.h>
#ifdef CONFIG_XENBUS
#include <mini-os/shutdown.h>
#endif
#include <mini-os/xmalloc.h>
#include <fcntl.h>
#include <xen/features.h>
#include <xen/version.h>

uint8_t xen_features[XENFEAT_NR_SUBMAPS * 32];
char cmdline[MAX_CMDLINE_SIZE];

void setup_xen_features(void)
{
    xen_feature_info_t fi;
    int i, j;

    for (i = 0; i < XENFEAT_NR_SUBMAPS; i++) 
    {
        fi.submap_idx = i;
        if (HYPERVISOR_xen_version(XENVER_get_features, &fi) < 0)
            break;
        
        for (j=0; j<32; j++)
            xen_features[i*32+j] = !!(fi.submap & 1<<j);
    }
}


/* This should be overridden by the application we are linked against. */
__attribute__((weak)) int app_main(void *p)
{
    printk("kernel.c: dummy main: par=%p\n", p);
    return 0;
}

void start_kernel(void* par)
{
    /* Set up events. */
    init_events();

    /* ENABLE EVENT DELIVERY. This is disabled at start of day. */
    local_irq_enable();

    setup_xen_features();

    /* Init memory management. */
    init_mm();

    /* Init time and timers. */
    init_time();

    /* Init the console driver. */
    init_console();

    /* Init grant tables */
    init_gnttab();
    
    /* Init scheduler. */
    init_sched();
 
    /* Init XenBus */
    init_xenbus();

#ifdef CONFIG_XENBUS
    /* Init shutdown thread */
    init_shutdown((start_info_t *)par);
#endif

    /* Call (possibly overridden) app_main() */
    app_main(NULL);

    /* Everything initialised, start idle thread */
    run_idle_thread();
}

void pre_suspend(void)
{
#ifdef CONFIG_NETFRONT
    suspend_netfront();
#endif

#ifdef CONFIG_XENBUS
    suspend_xenbus();
#endif

    local_irq_disable();

    suspend_gnttab();

    fini_time();

    suspend_console();

    suspend_events();
}

void post_suspend(int canceled)
{
    resume_console();

    init_time();

    resume_gnttab();

    local_irq_enable();

#ifdef CONFIG_XENBUS
    resume_xenbus(canceled);
#endif

#ifdef CONFIG_NETFRONT
    resume_netfront();
#endif
}

void stop_kernel(void)
{
    /* TODO: fs import */

    local_irq_disable();

    /* Reset grant tables */
    fini_gnttab();

    /* Reset XenBus */
    fini_xenbus();

    /* Reset timers */
    fini_time();

    /* Reset memory management. */
    fini_mm();

    /* Reset events. */
    fini_events();

    /* Reset arch details */
    arch_fini();
}

/*
 * do_exit: This is called whenever an IRET fails in entry.S.
 * This will generally be because an application has got itself into
 * a really bad state (probably a bad CS or SS). It must be killed.
 * Of course, minimal OS doesn't have applications :-)
 */

void do_exit(void)
{
    printk("Do_exit called!\n");
    arch_do_exit();
    for( ;; )
    {
        struct sched_shutdown sched_shutdown = { .reason = SHUTDOWN_crash };
        HYPERVISOR_sched_op(SCHEDOP_shutdown, &sched_shutdown);
    }
}
