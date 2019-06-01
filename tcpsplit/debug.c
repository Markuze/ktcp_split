#include <linux/string.h>
#include "rb_data_tree.h"
#include "tcp_split.h"
#include "rb_data_tree.h"
#include "cbn_common.h"
#include "debug.h"

#define STRLEN	256

extern struct rb_root listner_root;

static inline int dump_qp2user(char *str, struct cbn_qp *qp)
{
	return scnprintf(str, STRLEN,
				"[cpu = %d]:QP [%s:%s] %p: "TCP4" => "TCP4,
				smp_processor_id(),
				qp->qp_dir[TX_QP] ? "TX" : "",
				qp->qp_dir[RX_QP] ? "RX" : "",
				qp,
				TCP4N(&qp->addr_s, ntohs(qp->port_s)),
				TCP4N(&qp->addr_d, ntohs(qp->port_d)));
}

int dump_pending_connections(char __user *buff, size_t len, int loc, struct cbn_root_qp *qp_root)
{
	int rc = 0;
	struct rb_node *node = rb_first(&qp_root->root);
	char str[STRLEN];

	while (node) {
		struct cbn_qp *this = container_of(node, struct cbn_qp, node);

		rc = dump_qp2user(str, this);
		if (copy_to_user(buff + loc, str, rc))
			return -EFAULT;

		loc += rc;
		node = rb_next(node);
	}
	return loc;
}

int dump_connections_per_tennat(char __user *buff, size_t len, int loc, struct cbn_listner *listner)
{
	int cpu = 0, rc = 0;

	for_each_online_cpu(cpu) {
		struct cbn_root_qp *qp_root = per_cpu_ptr(listner->connections_root, cpu);
		//Print Pending QPs
		if ((rc = dump_pending_connections(buff, len, loc, qp_root)) < 0)
			return rc;
		loc += rc;
		//Add percore list and show paired QPs
	}
	return loc;
}

int dump_connections(char __user *buff, size_t len)
{
	int rc = 0, width = 0;
	char str[STRLEN];
	struct rb_node *node = rb_first(&listner_root);

	while (node) {
		struct cbn_listner *listner = rb_entry(node, struct cbn_listner, node);

		rc = scnprintf(str, STRLEN,
				"Tennat %d:\n--------------------------------------\n",
				listner->key);

		if (copy_to_user(buff + width, str, rc))
			return -EFAULT;

		if ((rc = dump_connections_per_tennat(buff, len, width, listner)) < 0)
			return rc;
		width += rc;
		node = rb_next(node);
	}
	return width;
}

