/* Minimal network driver for Mini-OS. 
 * Copyright (c) 2006-2007 Jacob Gorm Hansen, University of Copenhagen.
 * Based on netfront.c from Xen Linux.
 *
 * Does not handle fragments or extras.
 */

#include <mini-os/os.h>
#include <mini-os/xenbus.h>
#include <mini-os/events.h>
#include <errno.h>
#include <xen/io/netif.h>
#include <mini-os/gnttab.h>
#include <mini-os/xmalloc.h>
#include <mini-os/time.h>
#include <mini-os/netfront.h>
#include <mini-os/lib.h>
#include <mini-os/semaphore.h>

DECLARE_WAIT_QUEUE_HEAD(netfront_queue);

#ifdef HAVE_LIBC
#define NETIF_SELECT_RX ((void*)-1)
#endif



#define NET_TX_RING_SIZE __CONST_RING_SIZE(netif_tx, PAGE_SIZE)
#define NET_RX_RING_SIZE __CONST_RING_SIZE(netif_rx, PAGE_SIZE)
#define GRANT_INVALID_REF 0


struct net_buffer {
    void* page;
    grant_ref_t gref;
};

struct netfront_dev {
    domid_t dom;

    unsigned short tx_freelist[NET_TX_RING_SIZE + 1];
    struct semaphore tx_sem;

    struct net_buffer rx_buffers[NET_RX_RING_SIZE];
    struct net_buffer tx_buffers[NET_TX_RING_SIZE];

    struct netif_tx_front_ring tx;
    struct netif_rx_front_ring rx;
    grant_ref_t tx_ring_ref;
    grant_ref_t rx_ring_ref;
    evtchn_port_t evtchn;

    char *nodename;
    char *backend;
    char *mac;

    xenbus_event_queue events;

#ifdef HAVE_LIBC
    int fd;
    unsigned char *data;
    size_t len;
    size_t rlen;
#endif

    void (*netif_rx)(unsigned char* data, int len, void* arg);
    void *netif_rx_arg;
};

struct netfront_dev_list {
	struct netfront_dev *dev;
	unsigned char rawmac[6];
	char *ip;

	int refcount;

	struct netfront_dev_list *next;
};

static struct netfront_dev_list *dev_list = NULL;

void init_rx_buffers(struct netfront_dev *dev);
static struct netfront_dev *_init_netfront(struct netfront_dev *dev,
					   unsigned char rawmac[6], char **ip);
static void _shutdown_netfront(struct netfront_dev *dev);
void netfront_set_rx_handler(struct netfront_dev *dev,
			                 void (*thenetif_rx)(unsigned char *data, int len,
			                 void *arg), void *arg);

static inline void add_id_to_freelist(unsigned int id,unsigned short* freelist)
{
    freelist[id + 1] = freelist[0];
    freelist[0]  = id;
}

static inline unsigned short get_id_from_freelist(unsigned short* freelist)
{
    unsigned int id = freelist[0];
    freelist[0] = freelist[id + 1];
    return id;
}

__attribute__((weak)) void netif_rx(unsigned char* data, int len, void *arg)
{
    printk("%d bytes incoming at %p\n",len,data);
}

__attribute__((weak)) void net_app_main(void*si,unsigned char*mac) {}

static inline int xennet_rxidx(RING_IDX idx)
{
    return idx & (NET_RX_RING_SIZE - 1);
}

void network_rx(struct netfront_dev *dev)
{
    RING_IDX rp,cons,req_prod;
    int nr_consumed, more, i, notify;
    int dobreak;

    nr_consumed = 0;
moretodo:
    rp = dev->rx.sring->rsp_prod;
    rmb(); /* Ensure we see queued responses up to 'rp'. */

    dobreak = 0;
    for (cons = dev->rx.rsp_cons; cons != rp && !dobreak; nr_consumed++, cons++)
    {
        struct net_buffer* buf;
        unsigned char* page;
        int id;

        struct netif_rx_response *rx = RING_GET_RESPONSE(&dev->rx, cons);

        id = rx->id;
        BUG_ON(id >= NET_RX_RING_SIZE);

        buf = &dev->rx_buffers[id];
        page = (unsigned char*)buf->page;
        gnttab_end_access(buf->gref);

        if (rx->status > NETIF_RSP_NULL)
        {
#ifdef HAVE_LIBC
	    if (dev->netif_rx == NETIF_SELECT_RX) {
		int len = rx->status;
		ASSERT(current == main_thread);
		if (len > dev->len)
		    len = dev->len;
		memcpy(dev->data, page+rx->offset, len);
		dev->rlen = len;
		/* No need to receive the rest for now */
		dobreak = 1;
	    } else
#endif
		dev->netif_rx(page+rx->offset, rx->status, dev->netif_rx_arg);
        }
    }
    dev->rx.rsp_cons=cons;

    RING_FINAL_CHECK_FOR_RESPONSES(&dev->rx,more);
    if(more && !dobreak) goto moretodo;

    req_prod = dev->rx.req_prod_pvt;

    for(i=0; i<nr_consumed; i++)
    {
        int id = xennet_rxidx(req_prod + i);
        netif_rx_request_t *req = RING_GET_REQUEST(&dev->rx, req_prod + i);
        struct net_buffer* buf = &dev->rx_buffers[id];
        void* page = buf->page;

        /* We are sure to have free gnttab entries since they got released above */
        buf->gref = req->gref = 
            gnttab_grant_access(dev->dom,virt_to_mfn(page),0);

        req->id = id;
    }

    wmb();

    dev->rx.req_prod_pvt = req_prod + i;
    
    RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&dev->rx, notify);
    if (notify)
        notify_remote_via_evtchn(dev->evtchn);

}

void network_tx_buf_gc(struct netfront_dev *dev)
{


    RING_IDX cons, prod;
    unsigned short id;

    do {
        prod = dev->tx.sring->rsp_prod;
        rmb(); /* Ensure we see responses up to 'rp'. */

        for (cons = dev->tx.rsp_cons; cons != prod; cons++) 
        {
            struct netif_tx_response *txrsp;
            struct net_buffer *buf;

            txrsp = RING_GET_RESPONSE(&dev->tx, cons);
            if (txrsp->status == NETIF_RSP_NULL)
                continue;

            if (txrsp->status == NETIF_RSP_ERROR)
                printk("packet error\n");

            id  = txrsp->id;
            BUG_ON(id >= NET_TX_RING_SIZE);
            buf = &dev->tx_buffers[id];
            gnttab_end_access(buf->gref);
            buf->gref=GRANT_INVALID_REF;

	    add_id_to_freelist(id,dev->tx_freelist);
	    up(&dev->tx_sem);
        }

        dev->tx.rsp_cons = prod;

        /*
         * Set a new event, then check for race with update of tx_cons.
         * Note that it is essential to schedule a callback, no matter
         * how few tx_buffers are pending. Even if there is space in the
         * transmit ring, higher layers may be blocked because too much
         * data is outstanding: in such cases notification from Xen is
         * likely to be the only kick that we'll get.
         */
        dev->tx.sring->rsp_event =
            prod + ((dev->tx.sring->req_prod - prod) >> 1) + 1;
        mb();
    } while ((cons == prod) && (prod != dev->tx.sring->rsp_prod));


}

void netfront_handler(evtchn_port_t port, struct pt_regs *regs, void *data)
{
    int flags;
    struct netfront_dev *dev = data;

    local_irq_save(flags);

    network_tx_buf_gc(dev);
    network_rx(dev);

    local_irq_restore(flags);
}

#ifdef HAVE_LIBC
void netfront_select_handler(evtchn_port_t port, struct pt_regs *regs, void *data)
{
    int flags;
    struct netfront_dev *dev = data;
    int fd = dev->fd;

    local_irq_save(flags);
    network_tx_buf_gc(dev);
    local_irq_restore(flags);

    if (fd != -1)
        files[fd].read = 1;
    wake_up(&netfront_queue);
}
#endif

static void free_netfront(struct netfront_dev *dev)
{
    int i;

    for(i=0;i<NET_TX_RING_SIZE;i++)
	down(&dev->tx_sem);

    mask_evtchn(dev->evtchn);

    free(dev->mac);
    free(dev->backend);

    gnttab_end_access(dev->rx_ring_ref);
    gnttab_end_access(dev->tx_ring_ref);

    free_page(dev->rx.sring);
    free_page(dev->tx.sring);

    unbind_evtchn(dev->evtchn);

    for(i=0;i<NET_RX_RING_SIZE;i++) {
	gnttab_end_access(dev->rx_buffers[i].gref);
	free_page(dev->rx_buffers[i].page);
    }

    for(i=0;i<NET_TX_RING_SIZE;i++)
	if (dev->tx_buffers[i].page)
	    free_page(dev->tx_buffers[i].page);

    free(dev->nodename);
    free(dev);
}

struct netfront_dev *init_netfront(char *_nodename,
                                   void (*thenetif_rx)(unsigned char* data,
                                                       int len, void* arg),
                                   unsigned char rawmac[6],
                                   char **ip)
{
    char nodename[256];
    struct netfront_dev *dev;
    struct netfront_dev_list *ldev = NULL;
    struct netfront_dev_list *list = NULL;
    static int netfrontends = 0;

    if (!_nodename)
        snprintf(nodename, sizeof(nodename), "device/vif/%d", netfrontends);
    else {
        strncpy(nodename, _nodename, sizeof(nodename) - 1);
        nodename[sizeof(nodename) - 1] = 0;
    }

    /* Check if the device is already initialized */
    for (list = dev_list; list != NULL; list = list->next) {
        if (strcmp(nodename, list->dev->nodename) == 0) {
            list->refcount++;
            dev = list->dev;
            if (thenetif_rx)
                netfront_set_rx_handler(dev, thenetif_rx, NULL);
            goto out;
        }
    }

    if (!thenetif_rx)
        thenetif_rx = netif_rx;

    printk("************************ NETFRONT for %s **********\n\n\n", nodename);

    dev = malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));
    dev->nodename = strdup(nodename);
#ifdef HAVE_LIBC
    dev->fd = -1;
#endif
    dev->netif_rx = thenetif_rx;

    ldev = malloc(sizeof(struct netfront_dev_list));
    memset(ldev, 0, sizeof(struct netfront_dev_list));

    if (_init_netfront(dev, ldev->rawmac, &(ldev->ip))) {
	    ldev->dev = dev;
		ldev->refcount = 1;
		ldev->next = NULL;

		if (!dev_list) {
            dev_list = ldev;
		} else {
            for (list = dev_list; list->next != NULL; list = list->next);
            list->next = ldev;
		}
        netfrontends++;
	} else {
        free(dev->nodename);
        free(dev);
        free(ldev);
        dev = NULL;
        goto err;
    }

out:
    if (rawmac) {
        rawmac[0] = ldev->rawmac[0];
        rawmac[1] = ldev->rawmac[1];
        rawmac[2] = ldev->rawmac[2];
        rawmac[3] = ldev->rawmac[3];
        rawmac[4] = ldev->rawmac[4];
        rawmac[5] = ldev->rawmac[5];
	}
    if (ip) {
	    *ip = malloc(strlen(ldev->ip) + 1);
	    strncpy(*ip, ldev->ip, strlen(ldev->ip) + 1);
    }

err:
    return dev;
}

static struct netfront_dev *_init_netfront(struct netfront_dev *dev,
					   unsigned char rawmac[6],
					   char **ip)
{
    xenbus_transaction_t xbt;
    char* err = NULL;
    char* message=NULL;
    struct netif_tx_sring *txs;
    struct netif_rx_sring *rxs;
    char* msg = NULL;
    int retry=0;
    int i;
    char path[256];

    printk("net TX ring size %lu\n", (unsigned long) NET_TX_RING_SIZE);
    printk("net RX ring size %lu\n", (unsigned long) NET_RX_RING_SIZE);
    init_SEMAPHORE(&dev->tx_sem, NET_TX_RING_SIZE);
    for (i = 0; i < NET_TX_RING_SIZE; i++) {
	    add_id_to_freelist(i,dev->tx_freelist);
        dev->tx_buffers[i].page = NULL;
    }

    for (i = 0; i < NET_RX_RING_SIZE; i++) {
	    /* TODO: that's a lot of memory */
        dev->rx_buffers[i].page = (char*)alloc_page();
    }

    snprintf(path, sizeof(path), "%s/backend-id", dev->nodename);
    dev->dom = xenbus_read_integer(path);
#ifdef HAVE_LIBC
    if (thenetif_rx == NETIF_SELECT_RX)
        evtchn_alloc_unbound(dev->dom, netfront_select_handler, dev, &dev->evtchn);
    else
#endif
        evtchn_alloc_unbound(dev->dom, netfront_handler, dev, &dev->evtchn);

    txs = (struct netif_tx_sring *) alloc_page();
    rxs = (struct netif_rx_sring *) alloc_page();
    memset(txs, 0, PAGE_SIZE);
    memset(rxs, 0, PAGE_SIZE);

    SHARED_RING_INIT(txs);
    SHARED_RING_INIT(rxs);
    FRONT_RING_INIT(&dev->tx, txs, PAGE_SIZE);
    FRONT_RING_INIT(&dev->rx, rxs, PAGE_SIZE);

    dev->tx_ring_ref = gnttab_grant_access(dev->dom, virt_to_mfn(txs), 0);
    dev->rx_ring_ref = gnttab_grant_access(dev->dom, virt_to_mfn(rxs), 0);

    init_rx_buffers(dev);

    dev->events = NULL;

again:
    err = xenbus_transaction_start(&xbt);
    if (err) {
        printk("starting transaction\n");
        free(err);
    }

    err = xenbus_printf(xbt, dev->nodename, "tx-ring-ref","%u",
                        dev->tx_ring_ref);
    if (err) {
        message = "writing tx ring-ref";
        goto abort_transaction;
    }
    err = xenbus_printf(xbt, dev->nodename, "rx-ring-ref","%u",
                        dev->rx_ring_ref);
    if (err) {
        message = "writing rx ring-ref";
        goto abort_transaction;
    }
    err = xenbus_printf(xbt, dev->nodename, "event-channel", "%u", dev->evtchn);
    if (err) {
        message = "writing event-channel";
        goto abort_transaction;
    }

    err = xenbus_printf(xbt, dev->nodename, "request-rx-copy", "%u", 1);
    if (err) {
        message = "writing request-rx-copy";
        goto abort_transaction;
    }

    snprintf(path, sizeof(path), "%s/state", dev->nodename);
    err = xenbus_switch_state(xbt, path, XenbusStateConnected);
    if (err) {
        message = "switching state";
        goto abort_transaction;
    }

    err = xenbus_transaction_end(xbt, 0, &retry);
    free(err);
    if (retry) {
        goto again;
        printk("completing transaction\n");
    }

    goto done;

abort_transaction:
    free(err);
    err = xenbus_transaction_end(xbt, 1, &retry);
    printk("Abort transaction %s\n", message);
    goto error;

done:
    snprintf(path, sizeof(path), "%s/backend", dev->nodename);
    msg = xenbus_read(XBT_NIL, path, &dev->backend);
    snprintf(path, sizeof(path), "%s/mac", dev->nodename);
    msg = xenbus_read(XBT_NIL, path, &dev->mac);

    if ((dev->backend == NULL) || (dev->mac == NULL)) {
        printk("%s: backend/mac failed\n", __func__);
        goto error;
    }

    printk("backend at %s\n", dev->backend);
    printk("mac is %s\n", dev->mac);

    {
        XenbusState state;
        char path[strlen(dev->backend) + strlen("/state") + 1];
        snprintf(path, sizeof(path), "%s/state", dev->backend);

        xenbus_watch_path_token(XBT_NIL, path, path, &dev->events);

        err = NULL;
        state = xenbus_read_integer(path);
        while (err == NULL && state < XenbusStateConnected)
            err = xenbus_wait_for_state_change(path, &state, &dev->events);
        if (state != XenbusStateConnected) {
            printk("backend not avalable, state=%d\n", state);
            xenbus_unwatch_path_token(XBT_NIL, path, path);
            goto error;
        }

        if (ip) {
            snprintf(path, sizeof(path), "%s/ip", dev->backend);
            xenbus_read(XBT_NIL, path, ip);
        }
    }

    printk("**************************\n");

    unmask_evtchn(dev->evtchn);

    /* Special conversion specifier 'hh' needed for __ia64__. Without
     * this mini-os panics with 'Unaligned reference'.
     */
    if (rawmac)
	    sscanf(dev->mac,"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &rawmac[0],
               &rawmac[1],
               &rawmac[2],
               &rawmac[3],
               &rawmac[4],
               &rawmac[5]);

    return dev;

error:
    free(msg);
    free(err);
    free_netfront(dev);
    return NULL;
}

#ifdef HAVE_LIBC
int netfront_tap_open(char *nodename) {
    struct netfront_dev *dev;

    dev = init_netfront(nodename, NETIF_SELECT_RX, NULL, NULL);
    if (!dev) {
	printk("TAP open failed\n");
	errno = EIO;
	return -1;
    }
    dev->fd = alloc_fd(FTYPE_TAP);
    printk("tap_open(%s) -> %d\n", nodename, dev->fd);
    files[dev->fd].tap.dev = dev;
    return dev->fd;
}
#endif

void shutdown_netfront(struct netfront_dev *dev)
{
    struct netfront_dev_list *list = NULL;
    struct netfront_dev_list *to_del = NULL;

    /* Check this is a valid device */
    for (list = dev_list; list != NULL; list = list->next) {
	    if (list->dev == dev)
		    break;
    }

    if (!list) {
	    printk("Trying to shutdown an invalid netfront device (%p)\n", dev);
        return;
    }

    list->refcount--;
    if (list->refcount == 0) {
	    _shutdown_netfront(dev);
	    free(dev->nodename);
	    free(dev);

	    to_del = list;
        if (to_del == dev_list) {
            free(to_del);
			dev_list = NULL;
        } else {
	        for (list = dev_list; list->next != to_del; list = list->next);
	        list->next = to_del->next;
            free(to_del);
        }
    }
}

static void _shutdown_netfront(struct netfront_dev *dev)
{
    char* err = NULL, *err2;
    XenbusState state;

    char path[strlen(dev->backend) + strlen("/state") + 1];
    char nodename[strlen(dev->nodename) + strlen("/request-rx-copy") + 1];

    printk("close network: backend at %s\n",dev->backend);

    snprintf(path, sizeof(path), "%s/state", dev->backend);
    snprintf(nodename, sizeof(nodename), "%s/state", dev->nodename);

    if ((err = xenbus_switch_state(XBT_NIL, nodename, XenbusStateClosing)) != NULL) {
        printk("shutdown_netfront: error changing state to %d: %s\n",
                XenbusStateClosing, err);
        goto close;
    }
    state = xenbus_read_integer(path);
    while (err == NULL && state < XenbusStateClosing)
        err = xenbus_wait_for_state_change(path, &state, &dev->events);
    free(err);

    if ((err = xenbus_switch_state(XBT_NIL, nodename, XenbusStateClosed)) != NULL) {
        printk("shutdown_netfront: error changing state to %d: %s\n",
                XenbusStateClosed, err);
        goto close;
    }
    state = xenbus_read_integer(path);
    while (state < XenbusStateClosed) {
        err = xenbus_wait_for_state_change(path, &state, &dev->events);
        free(err);
    }

    if ((err = xenbus_switch_state(XBT_NIL, nodename, XenbusStateInitialising)) != NULL) {
        printk("shutdown_netfront: error changing state to %d: %s\n",
                XenbusStateInitialising, err);
        goto close;
    }
    state = xenbus_read_integer(path);
    while (err == NULL && (state < XenbusStateInitWait || state >= XenbusStateClosed))
        err = xenbus_wait_for_state_change(path, &state, &dev->events);

close:
    free(err);
    err2 = xenbus_unwatch_path_token(XBT_NIL, path, path);
    free(err2);

    snprintf(nodename, sizeof(nodename), "%s/tx-ring-ref", dev->nodename);
    err2 = xenbus_rm(XBT_NIL, nodename);
    free(err2);
    snprintf(nodename, sizeof(nodename), "%s/rx-ring-ref", dev->nodename);
    err2 = xenbus_rm(XBT_NIL, nodename);
    free(err2);
    snprintf(nodename, sizeof(nodename), "%s/event-channel", dev->nodename);
    err2 = xenbus_rm(XBT_NIL, nodename);
    free(err2);
    snprintf(nodename, sizeof(nodename), "%s/request-rx-copy", dev->nodename);
    err2 = xenbus_rm(XBT_NIL, nodename);
    free(err2);

    if (!err)
        free_netfront(dev);
}

void suspend_netfront(void)
{
    struct netfront_dev_list *list;

    for (list = dev_list; list != NULL; list = list->next)
        _shutdown_netfront(list->dev);
}

void resume_netfront(void)
{
    struct netfront_dev_list *list;

    for (list = dev_list; list != NULL; list = list->next)
        _init_netfront(list->dev, NULL, NULL);
}

void init_rx_buffers(struct netfront_dev *dev)
{
    int i, requeue_idx;
    netif_rx_request_t *req;
    int notify;

    /* Rebuild the RX buffer freelist and the RX ring itself. */
    for (requeue_idx = 0, i = 0; i < NET_RX_RING_SIZE; i++) 
    {
        struct net_buffer* buf = &dev->rx_buffers[requeue_idx];
        req = RING_GET_REQUEST(&dev->rx, requeue_idx);

        buf->gref = req->gref = 
            gnttab_grant_access(dev->dom,virt_to_mfn(buf->page),0);

        req->id = requeue_idx;

        requeue_idx++;
    }

    dev->rx.req_prod_pvt = requeue_idx;

    RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&dev->rx, notify);

    if (notify) 
        notify_remote_via_evtchn(dev->evtchn);

    dev->rx.sring->rsp_event = dev->rx.rsp_cons + 1;
}


void netfront_xmit(struct netfront_dev *dev, unsigned char* data,int len)
{
    int flags;
    struct netif_tx_request *tx;
    RING_IDX i;
    int notify;
    unsigned short id;
    struct net_buffer* buf;
    void* page;

    BUG_ON(len > PAGE_SIZE);

    down(&dev->tx_sem);

    local_irq_save(flags);
    id = get_id_from_freelist(dev->tx_freelist);
    local_irq_restore(flags);

    buf = &dev->tx_buffers[id];
    page = buf->page;
    if (!page)
	page = buf->page = (char*) alloc_page();

    i = dev->tx.req_prod_pvt;
    tx = RING_GET_REQUEST(&dev->tx, i);

    memcpy(page,data,len);

    buf->gref = 
        tx->gref = gnttab_grant_access(dev->dom,virt_to_mfn(page),1);

    tx->offset=0;
    tx->size = len;
    tx->flags=0;
    tx->id = id;
    dev->tx.req_prod_pvt = i + 1;

    wmb();

    RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&dev->tx, notify);

    if(notify) notify_remote_via_evtchn(dev->evtchn);

    local_irq_save(flags);
    network_tx_buf_gc(dev);
    local_irq_restore(flags);
}

#ifdef HAVE_LIBC
ssize_t netfront_receive(struct netfront_dev *dev, unsigned char *data, size_t len)
{
    unsigned long flags;
    int fd = dev->fd;
    ASSERT(current == main_thread);

    dev->rlen = 0;
    dev->data = data;
    dev->len = len;

    local_irq_save(flags);
    network_rx(dev);
    if (!dev->rlen && fd != -1)
	/* No data for us, make select stop returning */
	files[fd].read = 0;
    /* Before re-enabling the interrupts, in case a packet just arrived in the
     * meanwhile. */
    local_irq_restore(flags);

    dev->data = NULL;
    dev->len = 0;

    return dev->rlen;
}
#endif

void netfront_set_rx_handler(struct netfront_dev *dev,
			                 void (*thenetif_rx)(unsigned char *data, int len,
			                 void *arg), void *arg)
{
    if (dev->netif_rx && dev->netif_rx != netif_rx)
        printk("Replacing netif_rx handler for dev %s\n", dev->nodename);

    dev->netif_rx = thenetif_rx;
    dev->netif_rx_arg = arg;
}
