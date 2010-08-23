/* Copyright (C) 2003-2010 Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* Kernel module implementing an IP set type: the hash:ip,port type */

#include <linux/netfilter/ip_set_kernel.h>
#include <linux/netfilter/ip_set_jhash.h>
#include <linux/module.h>
#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/errno.h>
#include <asm/uaccess.h>
#include <asm/bitops.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/netlink.h>
#include <net/pfxlen.h>

#include <linux/netfilter.h>
#include <linux/netfilter/ip_set.h>
#include <linux/netfilter/ip_set_timeout.h>
#include <linux/netfilter/ip_set_getport.h>
#include <linux/netfilter/ip_set_hash.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>");
MODULE_DESCRIPTION("hash:ip,port type of IP sets");
MODULE_ALIAS("ip_set_hash:ip,port");

/* Type specific function prefix */
#define TYPE		hash_ipport

static bool
hash_ipport_same_set(const struct ip_set *a, const struct ip_set *b);

#define hash_ipport4_same_set	hash_ipport_same_set
#define hash_ipport6_same_set	hash_ipport_same_set

/* The type variant functions: IPv4 */

/* Member elements without timeout */
struct hash_ipport4_elem {
	u32 ip;
	u16 port;
	u8 proto;
	u8 padding;
};

/* Member elements with timeout support */
struct hash_ipport4_telem {
	u32 ip;
	u16 port;
	u8 proto;
	u8 padding;
	unsigned long timeout;
};

static inline bool
hash_ipport4_data_equal(const struct hash_ipport4_elem *ip1,
			const struct hash_ipport4_elem *ip2)
{
	return ip1->ip == ip2->ip
	       && ip1->port == ip2->port
	       && ip1->proto == ip2->proto;
}

static inline bool
hash_ipport4_data_isnull(const struct hash_ipport4_elem *elem)
{
	return elem->proto == 0;
}

static inline void
hash_ipport4_data_copy(struct hash_ipport4_elem *dst,
		       const struct hash_ipport4_elem *src)
{
	dst->ip = src->ip;
	dst->port = src->port;
	dst->proto = src->proto;
}

static inline void
hash_ipport4_data_swap(struct hash_ipport4_elem *dst,
		       struct hash_ipport4_elem *src)
{
	swap(dst->ip, src->ip);
	swap(dst->port, src->port);
	swap(dst->proto, src->proto);
}

static inline void
hash_ipport4_data_zero_out(struct hash_ipport4_elem *elem)
{
	elem->proto = 0;
}

static inline bool
hash_ipport4_data_list(struct sk_buff *skb,
		       const struct hash_ipport4_elem *data)
{
	NLA_PUT_NET32(skb, IPSET_ATTR_IP, data->ip);
	NLA_PUT_NET16(skb, IPSET_ATTR_PORT, data->port);
	if (data->proto != IPSET_IPPROTO_TCPUDP)
		NLA_PUT_U8(skb, IPSET_ATTR_PROTO, data->proto);
	return 0;

nla_put_failure:
	return 1;
}

static inline bool
hash_ipport4_data_tlist(struct sk_buff *skb,
			const struct hash_ipport4_elem *data)
{
	const struct hash_ipport4_telem *tdata =
		(const struct hash_ipport4_telem *)data;

	NLA_PUT_NET32(skb, IPSET_ATTR_IP, tdata->ip);
	NLA_PUT_NET16(skb, IPSET_ATTR_PORT, tdata->port);
	if (data->proto != IPSET_IPPROTO_TCPUDP)
		NLA_PUT_U8(skb, IPSET_ATTR_PROTO, data->proto);
	NLA_PUT_NET32(skb, IPSET_ATTR_TIMEOUT,
		      htonl(ip_set_timeout_get(tdata->timeout)));

	return 0;

nla_put_failure:
	return 1;
}

#define IP_SET_HASH_WITH_PROTO

#define PF		4
#define HOST_MASK	32
#include <linux/netfilter/ip_set_chash.h>

static int
hash_ipport4_kadt(struct ip_set *set, const struct sk_buff *skb,
		  enum ipset_adt adt, u8 pf, u8 dim, u8 flags)
{
	struct chash *h = set->data;
	ipset_adtfn adtfn = set->variant->adt[adt];
	struct hash_ipport4_elem data = { .proto = h->proto };

	if (!get_ip4_port(skb, flags & IPSET_DIM_ONE_SRC,
			  &data.port, &data.proto))
		return -EINVAL;

	ip4addrptr(skb, flags & IPSET_DIM_ONE_SRC, &data.ip);

	return adtfn(set, &data, GFP_ATOMIC, h->timeout);
}

static const struct nla_policy
hash_ipport4_adt_policy[IPSET_ATTR_ADT_MAX + 1] __read_mostly = {
	[IPSET_ATTR_IP]		= { .type = NLA_U32 },
	[IPSET_ATTR_PORT]	= { .type = NLA_U16 },
	[IPSET_ATTR_PROTO]	= { .type = NLA_U8 },
	[IPSET_ATTR_TIMEOUT]	= { .type = NLA_U32 },
	[IPSET_ATTR_LINENO]	= { .type = NLA_U32 },
};

static int
hash_ipport4_uadt(struct ip_set *set, struct nlattr *head, int len,
		  enum ipset_adt adt, u32 *lineno, u32 flags)
{
	struct chash *h = set->data;
	struct nlattr *tb[IPSET_ATTR_ADT_MAX+1];
	ipset_adtfn adtfn = set->variant->adt[adt];
	struct hash_ipport4_elem data = { .proto = h->proto };
	u32 timeout = h->timeout;
	int ret;

	if (nla_parse(tb, IPSET_ATTR_ADT_MAX, head, len,
		      hash_ipport4_adt_policy))
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_LINENO])
		*lineno = nla_get_u32(tb[IPSET_ATTR_LINENO]);

	if (tb[IPSET_ATTR_IP])
		data.ip = ip_set_get_n32(tb[IPSET_ATTR_IP]);
	else
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_PORT])
		data.port = ip_set_get_n16(tb[IPSET_ATTR_PORT]);
	else
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_PROTO]) {
		data.proto = nla_get_u8(tb[IPSET_ATTR_PROTO]);
		
		if (data.proto == 0 || data.proto >= IPSET_IPPROTO_TCPUDP)
			return -IPSET_ERR_INVALID_PROTO;
	} else if (data.proto == IPSET_IPPROTO_ANY)
		return -IPSET_ERR_MISSING_PROTO;

	switch (data.proto) {
	case IPPROTO_UDP:
	case IPPROTO_TCP:
	case IPSET_IPPROTO_TCPUDP:
		break;
	default:
		data.port = 0;
		break;
	}

	if (tb[IPSET_ATTR_TIMEOUT]) {
		if (!with_timeout(h->timeout))
			return -IPSET_ERR_TIMEOUT;
		timeout = ip_set_timeout_uget(tb[IPSET_ATTR_TIMEOUT]);
	}

	ret = adtfn(set, &data, GFP_KERNEL, timeout);

	return ip_set_eexist(ret, flags) ? 0 : ret;
}

static bool
hash_ipport_same_set(const struct ip_set *a, const struct ip_set *b)
{
	struct chash *x = a->data;
	struct chash *y = b->data;
	
	/* Resizing changes htable_bits, so we ignore it */
	return x->maxelem == y->maxelem
	       && x->timeout == y->timeout
	       && x->proto == y->proto
	       && x->array_size == y->array_size
	       && x->chain_limit == y->chain_limit;
}

/* The type variant functions: IPv6 */

struct hash_ipport6_elem {
	union nf_inet_addr ip;
	u16 port;
	u8 proto;
	u8 padding;
};

struct hash_ipport6_telem {
	union nf_inet_addr ip;
	u16 port;
	u8 proto;
	u8 padding;
	unsigned long timeout;
};

static inline bool
hash_ipport6_data_equal(const struct hash_ipport6_elem *ip1,
			const struct hash_ipport6_elem *ip2)
{
	return ipv6_addr_cmp(&ip1->ip.in6, &ip2->ip.in6) == 0
	       && ip1->port == ip2->port
	       && ip1->proto == ip2->proto;
}

static inline bool
hash_ipport6_data_isnull(const struct hash_ipport6_elem *elem)
{
	return elem->proto == 0;
}

static inline void
hash_ipport6_data_copy(struct hash_ipport6_elem *dst,
		       const struct hash_ipport6_elem *src)
{
	memcpy(dst, src, sizeof(*dst));
}

static inline void
hash_ipport6_data_swap(struct hash_ipport6_elem *dst,
		       struct hash_ipport6_elem *src)
{
	struct hash_ipport6_elem tmp;

	memcpy(&tmp, dst, sizeof(tmp));
	memcpy(dst, src, sizeof(tmp));
	memcpy(src, &tmp, sizeof(tmp));
}

static inline void
hash_ipport6_data_zero_out(struct hash_ipport6_elem *elem)
{
	elem->proto = 0;
}

static inline bool
hash_ipport6_data_list(struct sk_buff *skb,
		       const struct hash_ipport6_elem *data)
{
	NLA_PUT(skb, IPSET_ATTR_IP, sizeof(struct in6_addr), &data->ip);
	NLA_PUT_NET16(skb, IPSET_ATTR_PORT, data->port);
	if (data->proto != IPSET_IPPROTO_TCPUDP)
		NLA_PUT_U8(skb, IPSET_ATTR_PROTO, data->proto);
	return 0;

nla_put_failure:
	return 1;
}

static inline bool
hash_ipport6_data_tlist(struct sk_buff *skb,
			const struct hash_ipport6_elem *data)
{
	const struct hash_ipport6_telem *e = 
		(const struct hash_ipport6_telem *)data;
	
	NLA_PUT(skb, IPSET_ATTR_IP, sizeof(struct in6_addr), &e->ip);
	NLA_PUT_NET16(skb, IPSET_ATTR_PORT, data->port);
	if (data->proto != IPSET_IPPROTO_TCPUDP)
		NLA_PUT_U8(skb, IPSET_ATTR_PROTO, data->proto);
	NLA_PUT_NET32(skb, IPSET_ATTR_TIMEOUT,
		      htonl(ip_set_timeout_get(e->timeout)));
	return 0;

nla_put_failure:
	return 1;
}

#undef PF
#undef HOST_MASK

#define PF		6
#define HOST_MASK	128
#include <linux/netfilter/ip_set_chash.h>

static int
hash_ipport6_kadt(struct ip_set *set, const struct sk_buff *skb,
		  enum ipset_adt adt, u8 pf, u8 dim, u8 flags)
{
	struct chash *h = set->data;
	ipset_adtfn adtfn = set->variant->adt[adt];
	struct hash_ipport6_elem data = { .proto = h->proto };

	if (!get_ip6_port(skb, flags & IPSET_DIM_ONE_SRC,
			  &data.port, &data.proto))
		return -EINVAL;

	ip6addrptr(skb, flags & IPSET_DIM_ONE_SRC, &data.ip.in6);

	return adtfn(set, &data, GFP_ATOMIC, h->timeout);
}

static const struct nla_policy
hash_ipport6_adt_policy[IPSET_ATTR_ADT_MAX + 1] __read_mostly = {
	[IPSET_ATTR_IP]		= { .type = NLA_BINARY,
				    .len = sizeof(struct in6_addr) },
	[IPSET_ATTR_PORT]	= { .type = NLA_U16 },
	[IPSET_ATTR_PROTO]	= { .type = NLA_U8 },
	[IPSET_ATTR_TIMEOUT]	= { .type = NLA_U32 },
	[IPSET_ATTR_LINENO]	= { .type = NLA_U32 },
};

static int
hash_ipport6_uadt(struct ip_set *set, struct nlattr *head, int len,
		  enum ipset_adt adt, u32 *lineno, u32 flags)
{
	struct chash *h = set->data;
	struct nlattr *tb[IPSET_ATTR_ADT_MAX+1];
	ipset_adtfn adtfn = set->variant->adt[adt];
	struct hash_ipport6_elem data = { .proto = h->proto };
	u32 timeout = h->timeout;
	int ret;

	if (nla_parse(tb, IPSET_ATTR_ADT_MAX, head, len,
		      hash_ipport6_adt_policy))
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_LINENO])
		*lineno = nla_get_u32(tb[IPSET_ATTR_LINENO]);

	if (tb[IPSET_ATTR_IP])
		memcpy(&data.ip, nla_data(tb[IPSET_ATTR_IP]),
		       sizeof(struct in6_addr));
	else
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_PORT])
		data.port = ip_set_get_n16(tb[IPSET_ATTR_PORT]);
	else
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_PROTO]) {
		data.proto = nla_get_u8(tb[IPSET_ATTR_PROTO]);

		if (data.proto == 0 || data.proto >= IPSET_IPPROTO_TCPUDP)
			return -IPSET_ERR_INVALID_PROTO;
	} else if (data.proto == IPSET_IPPROTO_ANY)
		return -IPSET_ERR_MISSING_PROTO;

	switch (data.proto) {
	case IPPROTO_UDP:
	case IPPROTO_TCP:
	case IPSET_IPPROTO_TCPUDP:
		break;
	default:
		data.port = 0;
		break;
	}

	if (tb[IPSET_ATTR_TIMEOUT]) {
		if (!with_timeout(h->timeout))
			return -IPSET_ERR_TIMEOUT;
		timeout = ip_set_timeout_uget(tb[IPSET_ATTR_TIMEOUT]);
	}

	ret = adtfn(set, &data, GFP_KERNEL, timeout);
	
	return ip_set_eexist(ret, flags) ? 0 : ret;
}

/* Create hash:ip type of sets */

static const struct nla_policy
hash_ipport_create_policy[IPSET_ATTR_CREATE_MAX+1] __read_mostly = {
	[IPSET_ATTR_HASHSIZE]	= { .type = NLA_U32 },
	[IPSET_ATTR_MAXELEM]	= { .type = NLA_U32 },
	[IPSET_ATTR_PROBES]	= { .type = NLA_U8 },
	[IPSET_ATTR_RESIZE]	= { .type = NLA_U8  },
	[IPSET_ATTR_PROTO]	= { .type = NLA_U8 },
	[IPSET_ATTR_TIMEOUT]	= { .type = NLA_U32 },
};

static int
hash_ipport_create(struct ip_set *set, struct nlattr *head, int len, u32 flags)
{
	struct nlattr *tb[IPSET_ATTR_CREATE_MAX+1];
	struct chash *h;
	u32 hashsize = IPSET_DEFAULT_HASHSIZE, maxelem = IPSET_DEFAULT_MAXELEM;
	u8 proto = IPSET_IPPROTO_TCPUDP;	/* Backward compatibility */

	if (!(set->family == AF_INET || set->family == AF_INET6))
		return -IPSET_ERR_INVALID_FAMILY;

	if (nla_parse(tb, IPSET_ATTR_CREATE_MAX, head, len,
		      hash_ipport_create_policy))
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_HASHSIZE]) {
		hashsize = ip_set_get_h32(tb[IPSET_ATTR_HASHSIZE]);
		if (hashsize < IPSET_MIMINAL_HASHSIZE)
			hashsize = IPSET_MIMINAL_HASHSIZE;
	}

	if (tb[IPSET_ATTR_MAXELEM])
		maxelem = ip_set_get_h32(tb[IPSET_ATTR_MAXELEM]);

	if (tb[IPSET_ATTR_PROTO]) {
		proto = nla_get_u8(tb[IPSET_ATTR_PROTO]);
		if (!proto)
			return -IPSET_ERR_INVALID_PROTO;
	}

	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	h->maxelem = maxelem;
	h->htable_bits = htable_bits(hashsize);
	h->array_size = CHASH_DEFAULT_ARRAY_SIZE;
	h->chain_limit = CHASH_DEFAULT_CHAIN_LIMIT;
	get_random_bytes(&h->initval, sizeof(h->initval));
	h->proto = proto;
	h->timeout = IPSET_NO_TIMEOUT;

	h->htable = ip_set_alloc(jhash_size(h->htable_bits) * sizeof(struct slist),
				 GFP_KERNEL);
	if (!h->htable) {
		kfree(h);
		return -ENOMEM;
	}

	set->data = h;

	if (tb[IPSET_ATTR_TIMEOUT]) {
		h->timeout = ip_set_timeout_uget(tb[IPSET_ATTR_TIMEOUT]);
		
		set->variant = set->family == AF_INET
			? &hash_ipport4_tvariant : &hash_ipport6_tvariant;

		if (set->family == AF_INET)
			hash_ipport4_gc_init(set);
		else
			hash_ipport6_gc_init(set);
	} else {
		set->variant = set->family == AF_INET
			? &hash_ipport4_variant : &hash_ipport6_variant;
	}
	
	pr_debug("create %s hashsize %u (%u) maxelem %u: %p(%p)",
		 set->name, jhash_size(h->htable_bits),
		 h->htable_bits, h->maxelem, set->data, h->htable);
	   
	return 0;
}

static struct ip_set_type hash_ipport_type = {
	.name		= "hash:ip,port",
	.protocol	= IPSET_PROTOCOL,
	.features	= IPSET_TYPE_IP | IPSET_TYPE_PORT,
	.dimension	= IPSET_DIM_TWO,
	.family		= AF_UNSPEC,
	.revision	= 0,
	.create		= hash_ipport_create,
	.me		= THIS_MODULE,
};

static int __init
hash_ipport_init(void)
{
	return ip_set_type_register(&hash_ipport_type);
}

static void __exit
hash_ipport_fini(void)
{
	ip_set_type_unregister(&hash_ipport_type);
}

module_init(hash_ipport_init);
module_exit(hash_ipport_fini);
