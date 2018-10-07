#include <linux/init.h>      // included for __init and __exit macros
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <net/sock.h>  //sock->to
#include "tcp_split.h"
#include "pool.h"
#include "proc.h"
#include "rb_data_tree.h"
#include "preconn_rb_tree.h"
#include "cbn_common.h"

#define PRECONN_PRINT(...)

extern uint32_t ip_transparent;

extern struct kthread_pool cbn_pool;
extern struct kmem_cache *qp_slab;
extern struct kmem_cache *syn_slab;
extern struct kmem_cache *probe_slab;
extern struct rb_root listner_root;

static struct list_head pre_conn_list_server;
static struct list_head pre_conn_list_client;
static struct kmem_cache *preconn_slab;
static struct rb_root preconn_root = RB_ROOT;

static int prealloc_connection(void *arg);

static inline struct cbn_qp *alloc_prexeisting_conn(__be32 ip)
{
	struct cbn_qp *elem;
	struct list_head *pre_conn_list;
	unsigned long next_hop_ip = ip;
	struct cbn_preconnection *preconn = search_rb_preconn(&preconn_root, ip);

	pre_conn_list = (preconn) ? &preconn->list : NULL;

	if (unlikely(!pre_conn_list || list_empty(pre_conn_list))) {
		pr_err("preconn pool is empty! "TCP4", spawning refill...\n", TCP4N(&next_hop_ip, PRECONN_SERVER_PORT));
		kthread_pool_run(&cbn_pool, prealloc_connection, (void *)next_hop_ip);
		kthread_pool_run(&cbn_pool, prealloc_connection, (void *)next_hop_ip);
		return NULL;
	}

	elem = list_first_entry(pre_conn_list, struct cbn_qp, list);
	list_del(&elem->list);
	kthread_pool_run(&cbn_pool, prealloc_connection, (void *)next_hop_ip);
	return elem;
}

static inline int forward_conn_info(struct socket *tx, struct addresses *addresses)
{
	int rc;
	struct msghdr msg = { 0 };
	struct kvec kvec[1];

	kvec[0].iov_base = addresses;
	kvec[0].iov_len = sizeof(struct addresses);

	if ((rc = kernel_sendmsg(tx, &msg, kvec, 1, sizeof(struct addresses))) <= 0) {
		pr_err("Failed to forward info to next hop!\n");
	}
	return rc;
}

int start_new_pre_connection_syn(void *arg)
{
	int rc = 0;
	int line = __LINE__;
	struct addresses *addresses = arg;
	struct cbn_listner *listner;
	struct cbn_qp *qp, *tqp;
	struct sockets sockets;

	line = __LINE__;
	qp = alloc_prexeisting_conn(addresses->sin_addr.s_addr);
	if (!qp) {
		PRECONN_PRINT("Couldnt alloc a pre_connection to "TCP4, TCP4N(&addresses->sin_addr.s_addr, PRECONN_SERVER_PORT));
		start_new_connection_syn(arg);
		goto out;
	}
	/* check for failure + fallback to start_new_connection_syn...*/
	qp->addr_d = addresses->dest.sin_addr;
	qp->port_s = addresses->src.sin_port;
	qp->port_d = addresses->dest.sin_port;
	qp->addr_s = addresses->src.sin_addr;

	line = __LINE__;
	listner = search_rb_listner(&listner_root, addresses->mark);
	if (unlikely(!listner)) {
		PRECONN_PRINT("Listner missing %d, going out", addresses->mark);
		goto out;
	}
	qp->root = &listner->connections_root;

	PRECONN_PRINT("connection to "TCP4" from "TCP4,
			TCP4N(&qp->addr_d, ntohs(qp->port_d)), TCP4N(&qp->addr_s, ntohs(qp->port_s)));

	tqp = qp_exists(qp, TX_QP);
	line = __LINE__;
	if (unlikely(tqp == NULL)) {
		PRECONN_PRINT("Double ack... going out...\n");
		/*TODO: free qp...*/
		goto out;
	}
	line = __LINE__;
	if ((rc	= forward_conn_info((struct socket *)qp->tx, addresses)) <= 0) {
		PRECONN_PRINT("Failed to forward pre_connection to "TCPP4, TCPP4N(&addresses->sin_addr.s_addr, PRECONN_SERVER_PORT));
		start_new_connection_syn(arg);
		goto out;
	}

	kmem_cache_free(syn_slab, addresses);
	//TODO: add locks to this shit
	TRACE_LINE();
	if (wait_qp_ready(qp, TX_QP))
		goto out;

	TRACE_LINE();
	sockets.tx 	= (struct socket *)qp->rx;
	sockets.rx 	= (struct socket *)qp->tx;
	atomic_inc(&qp->ref_cnt);
	PRECONN_PRINT("starting half duplex %d", atomic_read(&qp->ref_cnt));
	rc = half_duplex(&sockets, qp);

	sock_release((struct socket *)qp->tx);
	if (rc)
		PRECONN_PRINT("OUT: Error %d on %d", rc, line);
out:
	return rc;
}

static inline void fill_preconn_address(__be32 ip, struct addresses *addresses)
{
	//	ip should already be __be32
	//	addr.s_addr = htonl(ip);
	addresses->dest.sin_family = AF_INET;
	addresses->dest.sin_addr.s_addr = ip;
	addresses->dest.sin_port = htons(PRECONN_SERVER_PORT);
}

static inline int add_preconn_qp(struct cbn_qp *qp, struct rb_root *root)
{
	struct cbn_preconnection *precon  = get_rb_preconn(root, qp->addr_d.s_addr,
								preconn_slab, GFP_KERNEL);
	if (unlikely(!precon)) {
		pr_err("Failed to alloc memory for preconn "IP4"\n", IP4N(&qp->addr_d));
		return -1;
	}
	list_add(&qp->list, &precon->list);
	return 0;
}

static int prealloc_connection(void *arg)
{
	int rc, optval = 1;
	int line;
	long ip = (long) arg;
	struct addresses addresses_s = {0};
	struct addresses *addresses = &addresses_s;
	struct cbn_qp *qp;
	struct socket *tx;

	fill_preconn_address(ip, addresses);
	qp = kmem_cache_alloc(qp_slab, GFP_KERNEL);
	init_waitqueue_head(&qp->wait);
	atomic_set(&qp->ref_cnt, 0);
	qp->addr_d = addresses->dest.sin_addr;
	qp->port_d = addresses->dest.sin_port;

	PRECONN_PRINT("connection to "TCP4, TCP4N(&qp->addr_d, ntohs(qp->port_d)));
	line = __LINE__;
	if ((rc = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &tx)))
		goto out;

	addresses->mark = CBN_CORE_ROUTE_MARK;

	line = __LINE__;
	if ((rc = kernel_setsockopt(tx, SOL_SOCKET, SO_MARK, (char *)&addresses->mark, sizeof(u32))) < 0)
		goto connect_fail;

	line = __LINE__;
	if ((rc = kernel_setsockopt(tx, SOL_SOCKET, SO_KEEPALIVE, (char *)&optval, sizeof(int))) < 0)
		goto connect_fail;

	line = __LINE__;
	if ((rc = kernel_setsockopt(tx, SOL_TCP, TCP_NODELAY, (char *)&optval, sizeof(optval))) < 0)
		goto connect_fail;

	line = __LINE__;
	if ((rc = kernel_connect(tx, (struct sockaddr *)&addresses->dest, sizeof(struct sockaddr), 0)))
		goto connect_fail;

	qp->tx = tx;
	qp->rx = NULL;

	line = __LINE__;
	if (!add_preconn_qp(qp, &preconn_root))
		goto out;

connect_fail:
	sock_release(tx);
out:
	if (rc)
		PRECONN_PRINT("pre-connection out %s <%d @ %d>", __FUNCTION__, rc, line);
	return rc;
}

static inline int preconn_wait_for_next_hop(struct cbn_qp *qp,
					struct addresses *addresses)
{

	struct msghdr msg = { 0 };
	struct kvec kvec;
	int rc;

	kvec.iov_base = addresses;
	kvec.iov_len = sizeof(struct addresses);

	if ((rc = kernel_recvmsg((struct socket *)qp->rx, &msg, &kvec, 1, sizeof(struct addresses), MSG_WAITALL)) <= 0)
		goto err;
err:
	return rc;
}

static int start_half_duplex(void *arg)
{
	void **args = arg;
	struct cbn_qp *qp = args[1];
	//PRECONN_PRINT("starting half duplex");
	half_duplex(args[0], args[1]);
	//PRECONN_PRINT("Going out... waking pair");
	args[0] = NULL;
	wake_up(&qp->wait);
	return 0;
}

static int start_new_pending_connection(void *arg)
{
	int rc, optval = 1;
	int line;
	struct cbn_qp *qp = arg;
	struct addresses addresses_s;
	struct addresses *addresses;
	struct sockets sockets, sockets_tx;
	struct socket *tx;
	void *ptr_pair[2];

	line = __LINE__;
	if ((rc = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &tx)))
		goto create_fail;

	addresses = &addresses_s;
	line = __LINE__;
	if ((rc = preconn_wait_for_next_hop(qp, addresses)) <= 0) {
		pr_err("waiting for next hop failed %d\n", rc);
		goto create_fail;
	}

	/* TODO: allow pipelining  - check if next hop is also preconnected....*/
	PRECONN_PRINT("connection to "TCP4" from "TCP4,
			TCP4N(&addresses->dest.sin_addr, ntohs(addresses->dest.sin_port)),
			TCP4N(&addresses->src.sin_addr, ntohs(addresses->src.sin_port)));

	line = __LINE__;
	if ((rc = kernel_setsockopt(tx, SOL_TCP, TCP_NODELAY, (char *)&optval, sizeof(optval))) < 0)
		goto connect_fail;

	line = __LINE__;
	if ((rc = kernel_setsockopt(tx, SOL_SOCKET,
					SO_MARK, (char *)&addresses->mark, sizeof(u32))) < 0)
		goto connect_fail;


	line = __LINE__;
	if (ip_transparent) {
		if ((rc = kernel_setsockopt(tx, SOL_IP, IP_TRANSPARENT, (char *)&optval, sizeof(int))))
			goto connect_fail;
		/*TODO: set src port to 0*/
		if ((rc = kernel_bind(tx, (struct sockaddr *)&addresses->src, sizeof(struct sockaddr))))
			goto connect_fail;
	}

	addresses->dest.sin_family = AF_INET;
	line = __LINE__;
	if ((rc = kernel_connect(tx, (struct sockaddr *)&addresses->dest, sizeof(struct sockaddr), 0)))
		goto connect_fail;

	qp->tx = tx;

	TRACE_LINE();

	sockets.tx = (struct socket *)qp->rx;
	sockets.rx = (struct socket *)qp->tx;
	sockets.dir = 0;

	sockets_tx.rx = (struct socket *)qp->rx;
	sockets_tx.tx = (struct socket *)qp->tx;
	sockets_tx.dir = 1;

	ptr_pair[0] = &sockets_tx;
	ptr_pair[1] = qp;
	atomic_inc(&qp->ref_cnt);
	kthread_pool_run(&cbn_pool, start_half_duplex, ptr_pair);

	atomic_inc(&qp->ref_cnt);
	PRECONN_PRINT("starting half duplex %d", atomic_read(&qp->ref_cnt));
	half_duplex(&sockets, qp);
	/* Must wait for the other thread to end...*/
	rc = wait_event_interruptible_timeout(qp->wait, (ptr_pair[0] == NULL),5 * HZ);
	if (!rc)
		pr_err("T/O waiting on start_half_duplex");


connect_fail:
	sock_release(tx);
	sock_release((struct socket *)qp->rx);
create_fail:
	if (rc < 0)
		PRECONN_PRINT("OUT: connection failed %d [%d]", rc , line);
	return rc;
}

static struct cbn_listner *pre_conn_listner;

static void preconn_resgister_server(struct cbn_listner *server)
{
	pre_conn_listner 		= server;
}

struct socket *craete_prec_conn_probe(u32 mark)
{
	int rc = 0;
	int line;
	struct socket *sock;

	line = __LINE__;
	if ((rc = sock_create_kern(&init_net, PF_INET, SOCK_RAW, IPPROTO_TCP, &sock)))
		goto error;

	line = __LINE__;
	if ((rc = kernel_setsockopt(sock, SOL_SOCKET, SO_MARK, (char *)&mark, sizeof(u32))) < 0)
		goto error;

	line = __LINE__;
	if ((rc = kernel_setsockopt(sock, SOL_IP, IP_HDRINCL , (char *)&mark, sizeof(u32))))
		goto error;

	return sock;
error:
	pr_err("error %d in line %d\n", rc, line);
	return NULL;
}


int start_probe_syn(void *arg)
{
	int rc = 0;
	struct msghdr msg = { 0 };
	struct kvec kvec[2];
	struct sockaddr_in addr;
	struct probe *probe = (struct probe*)arg;

	kvec[0].iov_base = &probe->iphdr;
	kvec[0].iov_len = sizeof(struct iphdr);

	/*
	 * We need the original port number to find the pair QP
	 * we store it in the __be16 window field of the tcpheader
	 * to be extracted on cbn_egress_hook
	 * */
	probe->tcphdr.window = probe->tcphdr.source;
	probe->tcphdr.source = htons(CBN_PROBE_PORT);
	kvec[1].iov_base = &probe->tcphdr;
	kvec[1].iov_len = sizeof(struct tcphdr);

	/* Need to set dest addr here...*/
	addr.sin_family 	= AF_INET;
	addr.sin_addr.s_addr 	= probe->iphdr.daddr;
	addr.sin_port 		= 0;//probe->tcphdr.dest;

	msg.msg_namelen = sizeof(struct sockaddr_in);
	msg.msg_name = &addr;

	if ((rc = kernel_sendmsg(probe->listner->raw, &msg, kvec, 2,
				  sizeof(struct tcphdr) +
				  sizeof(struct iphdr))) <= 0) {
		/* FIXME: -1 will return if next dev is not gue+*/
		pr_err("Failed to send next hop %d\n", rc);
	}

	/* tcphdr & iphdr already copied...*/
	kmem_cache_free(probe_slab, probe);

	return rc;
}

#define BACKLOG 16

static int prec_conn_listner_server(void *arg)
{
	int rc = 0;
	struct socket *sock = NULL;
	struct sockaddr_in srv_addr;
	struct cbn_listner server_s;
	struct cbn_listner *server;
	u32 port;

	port = (long)arg;
	PRECONN_PRINT("Pre conn server running %d", port);
	if ((rc = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock)))
		goto error;

	server = &server_s;
	server->sock = sock;
	server->port = port;

	srv_addr.sin_family 		= AF_INET;
	srv_addr.sin_addr.s_addr 	= htonl(INADDR_ANY);
	srv_addr.sin_port 		= htons(port);

	if ((rc = kernel_bind(sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr))))
		goto error;

	port = CBN_CORE_ROUTE_MARK;
	if ((rc = kernel_setsockopt(sock, SOL_SOCKET, SO_MARK, (char *)&port, sizeof(u32))) < 0)
		goto error;

	if ((rc = kernel_listen(sock, BACKLOG)))
		goto error;

	PRECONN_PRINT("Listening to new pre connections");
	preconn_resgister_server(server);
	do {
		struct socket *nsock;
		struct cbn_qp *qp;

		rc = kernel_accept(sock, &nsock, 0);
		if (unlikely(rc)) {
			PRECONN_PRINT("rc is %d", rc);
			goto out;
		}

		PRECONN_PRINT("new pre connection...");
		qp = kmem_cache_alloc(qp_slab, GFP_KERNEL);
		atomic_set(&qp->ref_cnt, 0);
		init_waitqueue_head(&qp->wait);
		qp->rx 		= nsock;
		qp->tid 	= 0;
		qp->root 	= NULL;
		INIT_LIST_HEAD(&qp->list);

		list_add(&qp->list, &pre_conn_list_server);
		kthread_pool_run(&cbn_pool, start_new_pending_connection, qp);

	} while (!kthread_should_stop());
	error:
	pr_err("Exiting %d\n", rc);
	out:
	if (sock)
		sock_release(sock);
	return rc;
}

static inline int build_ip(int *array)
{
	int i, ip = 0;
	for (i = 0; i < 4; i++)
		if (array[i] > 255)
			return 0;
		else
			ip = (ip << 8)|array[i];
	return ip;
}

static void clear_client_pre_connections(void)
{
	struct list_head *itr, *tmp;

	list_for_each_safe(itr, tmp, &pre_conn_list_client) {
		struct cbn_qp *elem = container_of(itr, struct cbn_qp, list);
		list_del(itr);
		sock_release((struct socket *)elem->tx);
		kmem_cache_free(qp_slab, elem);
	}
}

void preconn_write_cb(int *array)
{
	long ip;

	ip = build_ip(array);
	if (!ip) {
		pr_err("ERROR: ip is invalid %d.%d.%d.%d\n",array[0], array[1], array[2], array[3]);
		return;
	}

	if (ip) {
		pr_info("connecting to %d.%d.%d.%d (%lx)\n", array[0], array[1], array[2], array[3], ip);
		ip = htonl(ip);
		kthread_pool_run(&cbn_pool, prealloc_connection, (void *)ip);
		kthread_pool_run(&cbn_pool, prealloc_connection, (void *)ip);
		kthread_pool_run(&cbn_pool, prealloc_connection, (void *)ip);
		kthread_pool_run(&cbn_pool, prealloc_connection, (void *)ip);
	}
}

int __init cbn_pre_connect_init(void)
{
	long port = PRECONN_SERVER_PORT;
	INIT_LIST_HEAD(&pre_conn_list_client);
	INIT_LIST_HEAD(&pre_conn_list_server);

	preconn_slab = kmem_cache_create("cbn_preconn_slab",
					 sizeof(struct cbn_preconnection), 0, 0, NULL);
	kthread_run(prec_conn_listner_server, (void *)port,"pre-conn-server");

	return 0;
}

int __exit cbn_pre_connect_end(void)
{
	struct list_head *itr, *tmp;

	PRECONN_PRINT("listner %p [%p]", pre_conn_listner, pre_conn_listner ? pre_conn_listner->sock: NULL);
	if (pre_conn_listner)
		kernel_sock_shutdown(pre_conn_listner->sock, SHUT_RDWR);

	PRECONN_PRINT("clear client connections");
	clear_client_pre_connections();

	PRECONN_PRINT("clear server connections");
	list_for_each_safe(itr, tmp, &pre_conn_list_server) {
		struct cbn_qp *elem = container_of(itr, struct cbn_qp, list);
		list_del(itr);
		sock_release((struct socket *)elem->rx);
		kmem_cache_free(qp_slab, elem);
	}
	kmem_cache_destroy(preconn_slab);
	return 0;
}
