#include "fp_driver.h"
#include <linux/srcu.h>

#define MAX_NL_MSG_LEN 16
static u32 fp_nl_portid;
static struct sock __rcu *nl_sk;
static DEFINE_MUTEX(fp_nl_lock);
DEFINE_STATIC_SRCU(fp_nl_srcu);

void fp_netlink_send(struct fp_device *fp_dev, const int command)
{
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	struct sock *sk;
	u32 portid;
	int idx;

	if (command < 0 || command > U8_MAX)
		return;
	idx = srcu_read_lock(&fp_nl_srcu);
	sk = srcu_dereference(nl_sk, &fp_nl_srcu);
	portid = READ_ONCE(fp_nl_portid);
	if (!sk || !portid)
		goto out;
	skb = nlmsg_new(MAX_NL_MSG_LEN, GFP_ATOMIC);
	if (!skb)
		goto out;
	nlh = nlmsg_put(skb, 0, 0, 0, MAX_NL_MSG_LEN, 0);
	if (!nlh) {
		kfree_skb(skb);
		goto out;
	}
	memset(nlmsg_data(nlh), 0, MAX_NL_MSG_LEN);
	*(u8 *)nlmsg_data(nlh) = command;
	NETLINK_CB(skb).portid = 0;
	NETLINK_CB(skb).dst_group = 0;
	netlink_unicast(sk, skb, portid, MSG_DONTWAIT);
out:
	srcu_read_unlock(&fp_nl_srcu, idx);
}

void fp_netlink_recv(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;
	struct sock *sk;
	int idx;

	if (!skb || skb->len < NLMSG_HDRLEN)
		return;
	nlh = nlmsg_hdr(skb);
	if (!nlmsg_ok(nlh, skb->len))
		return;
	idx = srcu_read_lock(&fp_nl_srcu);
	sk = srcu_dereference(nl_sk, &fp_nl_srcu);
	/* Ignore an old socket callback delayed across destroy and reinit. */
	if (sk && skb->sk == sk)
		WRITE_ONCE(fp_nl_portid, NETLINK_CB(skb).portid);
	srcu_read_unlock(&fp_nl_srcu, idx);
}

int fp_netlink_init(struct fp_device *fp_dev)
{
	struct netlink_kernel_cfg cfg = { .input = fp_netlink_recv };
	struct sock *sk;
	int ret = 0;

	mutex_lock(&fp_nl_lock);
	if (rcu_access_pointer(nl_sk))
		goto out;
	WRITE_ONCE(fp_nl_portid, 0);
	sk = netlink_kernel_create(&init_net, fp_dev->fp_netlink_num, &cfg);
	if (!sk) {
		ret = -ENOMEM;
		goto out;
	}
	rcu_assign_pointer(nl_sk, sk);
out:
	mutex_unlock(&fp_nl_lock);
	return ret;
}

int fp_netlink_destroy(struct fp_device *fp_dev)
{
	struct sock *sk;

	mutex_lock(&fp_nl_lock);
	sk = rcu_replace_pointer(nl_sk, NULL, lockdep_is_held(&fp_nl_lock));
	if (sk) {
		synchronize_srcu(&fp_nl_srcu);
		netlink_kernel_release(sk);
	}
	WRITE_ONCE(fp_nl_portid, 0);
	mutex_unlock(&fp_nl_lock);
	return 0;
}
