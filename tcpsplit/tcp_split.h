#ifndef __CBN_DATAPATH_H__
#define __CBN_DATAPATH_H__

#include <linux/percpu-rwsem.h>
#include "cbn_common.h"

#define QP_TO			90
#define PRECONN_SERVER_PORT	5565
#define CBN_CORE_ROUTE_MARK   ((UINT_MAX >> 1) + 1)   //0x80000000U

//Lowest official IANA unassigned port
#define CBP_PROBE_PORT 	4
#define RB_KEY_LENGTH 12

#define TX_QP	0
#define RX_QP	1

#define VANILA_KERNEL
#ifdef VANILA_KERNEL
#include <linux/kallsyms.h>

typedef long (*setaffinity_func)(pid_t, const struct cpumask *);
typedef void (*bind_mask_func)(struct task_struct *, const struct cpumask *);
typedef void __percpu *(*alloc_percpu_func)(size_t , size_t);

extern setaffinity_func psched_setaffinity;
extern bind_mask_func  pkthread_bind_mask;
extern alloc_percpu_func  p__alloc_reserved_per_cpu;

#define sched_setaffinity(...)		(*psched_setaffinity)(__VA_ARGS__)
#define kthread_bind_mask(...)		(*pkthread_bind_mask)(__VA_ARGS__)
#define __alloc_reserved_percpu(...)	(*p__alloc_reserved_percpu)(__VA_ARGS__)
#endif

struct cbn_root_qp {
	struct rb_root root;
};

struct cbn_listner {
	struct rb_node 	node;
	struct cbn_root_qp __percpu *connections_root; /* per core variable, sane goes for lock*/
	struct percpu_rw_semaphore rb_lock;
	int32_t		key; //tid
	uint16_t	port;
	uint16_t	status;
	struct socket	*sock;
	struct socket	*raw;
};

struct cbn_qp {
	struct rb_node node;
	union {
		char key[RB_KEY_LENGTH];
		struct {
			__be16		port_s;	/* Port number			*/
			__be16		port_d;	/* Port number			*/
			struct in_addr	addr_s;	/* Internet address		*/
			struct in_addr	addr_d;	/* Internet address		*/
		};
		struct {
			int tid;

		};
	};
	atomic_t ref_cnt;

	struct cbn_listner 	*listner;
	struct list_head 	list;
	wait_queue_head_t	wait;
	spinlock_t 		lock;
	union {
		struct {
			struct socket	*tx;
			struct socket	*rx;
		};
		struct socket *qp_dir[2]; //TODO: volatile
	};
};

extern struct kmem_cache *qp_slab;

static inline void dump_qp(struct cbn_qp *qp, const char *str)
{
	TRACE_QP("<%s> %s :QP %p: "TCP4" => "TCP4, __FUNCTION__, str, qp,
			TCP4N(&qp->addr_s, ntohs(qp->port_s)),
			TCP4N(&qp->addr_d, ntohs(qp->port_d)));
}

static inline void get_qp(struct cbn_qp *qp)
{
	int rc;
	rc = atomic_inc_return(&qp->ref_cnt);
	TRACE_DEBUG("%s : (%p->[%p][%p]) %d", __FUNCTION__, qp, qp->tx, qp->rx, rc);
	switch  (rc) {
	case 2:
		dump_qp(qp, "remove from tree");
		if (qp->listner) {
			struct cbn_root_qp *qp_root = this_cpu_ptr(qp->listner->connections_root);

			de_tree_qp(&qp->node, &qp_root->root, &qp->listner->rb_lock);
		}
		/* else is legitamate in start_new_pending_connection
		 */
		/*Intentional falltrough */
	case 1:
		spin_lock_init(&qp->lock);
		break;
	default:
		TRACE_ERROR("Impossible QP refcount %d", rc);
		dump_qp(qp, "IMPOSSIBLE VALUE");
		break;
	}
}

static inline unsigned int put_qp(struct cbn_qp *qp)
{
	int rc;
	unsigned long flags;

	/**
	 * TODO: This whole section must to be atomic, due to enumerable TOCTOU(Race condition) issues...
	 *	spin_lock is enough irq/bh contexts dont work with QPs.
	 */
	spin_lock_irqsave(&qp->lock, flags);
	if (! (rc = atomic_dec_return(&qp->ref_cnt))) {
		//TODO: Consider adding a tree for active QPs + States.
		//TODO: Add waitqueue here...
		if (!IS_ERR_OR_NULL(qp->tx))
			sock_release(qp->tx);
		if (!IS_ERR_OR_NULL(qp->rx))
			sock_release(qp->rx);
		kmem_cache_free(qp_slab, qp);
	} else {
		if (!IS_ERR_OR_NULL(qp->tx))
			kernel_sock_shutdown(qp->tx, SHUT_RDWR);
		if (!IS_ERR_OR_NULL(qp->rx))
			kernel_sock_shutdown(qp->rx, SHUT_RDWR);
	}
	spin_unlock_irqrestore(&qp->lock, flags);
	TRACE_DEBUG("%s : (%p->[%p][%p]) %d", __FUNCTION__, qp, qp->tx, qp->rx, rc);
	return rc;
}

struct sockets {
	struct socket *rx;
	struct socket *tx;
	int 	dir;
};

struct addresses {
	struct sockaddr_in dest;
	struct sockaddr_in src;
	struct in_addr	sin_addr;
	int mark;
};

struct probe {
	struct iphdr iphdr;
	struct tcphdr tcphdr;
	struct cbn_listner *listner;
};

#define UINT_SHIFT	32
static inline void* uint2void(uint32_t a, uint32_t b)
{
	return (void *)((((uint64_t)a)<<UINT_SHIFT)|b);
}

static inline void void2uint(void *ptr, uint32_t *a, uint32_t *b)
{
	uint64_t concat = (uint64_t)ptr;
	*b = ((concat << UINT_SHIFT) >> UINT_SHIFT);
	*a = (concat >> UINT_SHIFT);
}

static inline unsigned int qp2cpu(struct cbn_qp *qp)
{
	unsigned int i = 0;
	unsigned int core = 0;

	char str[32];

	for (;i < RB_KEY_LENGTH; i++)
		core ^= qp->key[i];
	i = num_online_cpus();
	i = (core/i) * i;

	snprintf(str, 32, "core = %u, i = %u => %u", core, i, core - i);
	dump_qp(qp, str);
	return core - i;
}

static inline unsigned int addresses2cpu(struct addresses *addr)
{
	struct cbn_qp qp;
	qp.addr_d = addr->dest.sin_addr;
	qp.port_s = addr->src.sin_port;
	qp.port_d = addr->dest.sin_port;
	qp.addr_s = addr->src.sin_addr;
	return qp2cpu(&qp);
}

int half_duplex(struct sockets *sock, struct cbn_qp *qp);

void add_server_cb(int tid, int port);
void del_server_cb(int tid);
void preconn_write_cb(int *);
inline char* proc_read_string(int *);

struct socket *craete_prec_conn_probe(u32 mark);

int __init cbn_pre_connect_init(void);
int __exit cbn_pre_connect_end(void);

int start_probe_syn(void *arg);
int start_new_connection_syn(void *arg);
inline int wait_qp_ready(struct cbn_qp* qp, uint8_t dir);
inline struct cbn_qp *qp_exists(struct cbn_qp* pqp, uint8_t dir);
#endif /*__CBN_DATAPATH_H__*/
