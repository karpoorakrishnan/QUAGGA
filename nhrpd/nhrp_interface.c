/* NHRP interface
 * Copyright (c) 2014-2015 Timo Teräs
 *
 * This file is free software: you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <net/if_arp.h>
#include "zebra.h"
#include "linklist.h"
#include "memory.h"
#include "thread.h"

#include "nhrpd.h"
#include "os.h"

static int nhrp_if_new_hook(struct interface *ifp)
{
	struct nhrp_interface *nifp;
	afi_t afi;

	nifp = XCALLOC(MTYPE_NHRP_IF, sizeof(struct nhrp_interface));
	if (!nifp) return 0;

	ifp->info = nifp;

	notifier_init(&nifp->notifier_list);
	for (afi = 0; afi < AFI_MAX; afi++) {
		struct nhrp_afi_data *ad = &nifp->afi[afi];
		ad->holdtime = NHRPD_DEFAULT_HOLDTIME;
		list_init(&ad->nhslist_head);
	}

	return 0;
}

static int nhrp_if_delete_hook(struct interface *ifp)
{
	struct nhrp_interface *nifp = ifp->info;
	struct nhrp_nhs *nhs, *tmp;
	afi_t afi;

	for (afi = 0; afi < AFI_MAX; afi++) {
		list_for_each_entry_safe(nhs, tmp, &nifp->afi[afi].nhslist_head, nhslist_entry)
			nhrp_nhs_free(nhs);
	}
	XFREE(MTYPE_NHRP_IF, ifp->info);
	return 0;
}

void nhrp_interface_init(void)
{
	if_init();
	if_add_hook(IF_NEW_HOOK,    nhrp_if_new_hook);
	if_add_hook(IF_DELETE_HOOK, nhrp_if_delete_hook);
}

void nhrp_interface_terminate(void)
{
	if_terminate();
}

static void nhrp_interface_interface_notifier(struct notifier_block *n, unsigned long cmd)
{
	struct nhrp_interface *nifp = container_of(n, struct nhrp_interface, nbmanifp_notifier);

	switch (cmd) {
	case NOTIFY_INTERFACE_ADDRESS_CHANGED:
		nifp->nbma = nifp->nbmanifp->afi[AFI_IP].addr;
		notifier_call(&nifp->notifier_list, NOTIFY_INTERFACE_NBMA_CHANGED);
		debugf(NHRP_DEBUG_IF, "NBMA address changed");
		break;
	}
}

static void nhrp_interface_update_nbma(struct interface *ifp)
{
	struct nhrp_interface *nifp = ifp->info, *nbmanifp = NULL;
	struct interface *nbmaifp = NULL;
	union sockunion nbma;

	sockunion_family(&nbma) = AF_UNSPEC;
	switch (ifp->hw_type) {
	case ARPHRD_IPGRE: {
			struct in_addr saddr = {0};
			os_get_mgre_config(ifp->name, &nifp->grekey, &nifp->linkidx, &saddr);
			debugf(NHRP_DEBUG_IF, "os_mgre: %x %x %x", nifp->grekey, nifp->linkidx, saddr.s_addr);
			if (saddr.s_addr)
				sockunion_set(&nbma, AF_INET, (u_char *) &saddr.s_addr, sizeof(saddr.s_addr));
			else if (nifp->linkidx != IFINDEX_INTERNAL)
				nbmaifp = if_lookup_by_index(nifp->linkidx);
		}
		break;
	}

	if (nbmaifp) nbmanifp = nbmaifp->info;

	if (nbmanifp != nifp->nbmanifp) {
		if (nifp->nbmanifp)
			notifier_del(&nifp->nbmanifp_notifier);
		nifp->nbmanifp = nbmanifp;
		if (nbmanifp) {
			notifier_add(&nifp->nbmanifp_notifier, &nbmanifp->notifier_list, nhrp_interface_interface_notifier);
			nbma = nbmanifp->afi[AFI_IP].addr;
			debugf(NHRP_DEBUG_IF, "%s: bound to %s", ifp->name, nbmaifp->name);
		}
	}

	if (!sockunion_same(&nbma, &nifp->nbma)) {
		nifp->nbma = nbma;
		debugf(NHRP_DEBUG_IF, "%s: NBMA address changed", ifp->name);
		notifier_call(&nifp->notifier_list, NOTIFY_INTERFACE_NBMA_CHANGED);
	}

	nhrp_interface_update(ifp);
}

static void nhrp_interface_update_address(struct interface *ifp, afi_t afi)
{
	const int family = afi2family(afi);
	struct nhrp_interface *nifp = ifp->info;
	struct nhrp_afi_data *if_ad = &nifp->afi[afi];
	struct nhrp_cache *nc;
	struct connected *c, *best;
	struct listnode *cnode;
	union sockunion addr;
	char buf[PREFIX_STRLEN];

	/* Select new best match preferring primary address */
	best = NULL;
	for (ALL_LIST_ELEMENTS_RO(ifp->connected, cnode, c)) {
		if (PREFIX_FAMILY(c->address) != family)
			continue;
		if (best == NULL) {
			best = c;
			continue;
		}
		if ((best->flags & ZEBRA_IFA_SECONDARY) && !(c->flags & ZEBRA_IFA_SECONDARY)) {
			best = c;
			continue;
		}
		if (!(best->flags & ZEBRA_IFA_SECONDARY) && (c->flags & ZEBRA_IFA_SECONDARY))
			continue;
		if (best->address->prefixlen > c->address->prefixlen) {
			best = c;
			continue;
		}
		if (best->address->prefixlen < c->address->prefixlen)
			continue;
	}

	/* On NHRP interfaces a host prefix is required */
	if (best && nifp->enabled && best->address->prefixlen != 8 * prefix_blen(best->address)) {
		zlog_notice("%s: %s is not a host prefix", ifp->name,
			prefix2str(best->address, buf, sizeof buf));
		best = NULL;
	}

	/* Update address if it changed */
	if (best)
		prefix2sockunion(best->address, &addr);
	else
		memset(&addr, 0, sizeof(addr));

	if (sockunion_same(&if_ad->addr, &addr))
		return;

	if (sockunion_family(&if_ad->addr) != AF_UNSPEC) {
		nc = nhrp_cache_get(ifp, &if_ad->addr, 0);
		if (nc) nhrp_cache_update_binding(nc, NHRP_CACHE_LOCAL, -1, NULL, NULL);
	}

	debugf(NHRP_DEBUG_KERNEL, "%s: IPv%d address changed to %s",
		ifp->name, afi == AFI_IP ? 4 : 6,
		best ? prefix2str(best->address, buf, sizeof buf) : "(none)");

	if_ad->addr = addr;

	if (nifp->enabled && sockunion_family(&if_ad->addr) != AF_UNSPEC) {
		nc = nhrp_cache_get(ifp, &addr, 1);
		if (nc) nhrp_cache_update_binding(nc, NHRP_CACHE_LOCAL, 0, NULL, NULL);
	}

	notifier_call(&nifp->notifier_list, NOTIFY_INTERFACE_ADDRESS_CHANGED);
}

void nhrp_interface_update(struct interface *ifp)
{
	struct nhrp_interface *nifp = ifp->info;
	struct nhrp_afi_data *if_ad;
	afi_t afi;
	int enabled = 0;

	if (sockunion_family(&nifp->nbma) == AF_UNSPEC)
		goto not_ok;
	if (ifp->ifindex == IFINDEX_INTERNAL)
		goto not_ok;

	for (afi = 0; afi < AFI_MAX; afi++) {
		if_ad = &nifp->afi[afi];

		if (!if_ad->network_id) {
			if_ad->configured = 0;
			continue;
		}

		if (!if_ad->configured) {
			os_configure_dmvpn(ifp->ifindex, ifp->name, afi2family(afi));
			if_ad->configured = 1;
		}
		nhrp_interface_update_address(ifp, afi);
		enabled = 1;
	}

not_ok:
	if (enabled != nifp->enabled) {
		nifp->enabled = enabled;
		notifier_call(&nifp->notifier_list, enabled ? NOTIFY_INTERFACE_UP : NOTIFY_INTERFACE_DOWN);
	}
}

int nhrp_interface_add(int cmd, struct zclient *client, zebra_size_t length)
{
	struct interface *ifp;

	/* read and add the interface in the iflist. */
	ifp = zebra_interface_add_read(client->ibuf);
	if (ifp == NULL)
		return 0;

	debugf(NHRP_DEBUG_IF, "if-add: %s, hw_type: %d", ifp->name, ifp->hw_type);
	nhrp_interface_update_nbma(ifp);

	return 0;
}

int nhrp_interface_delete(int cmd, struct zclient *client, zebra_size_t length)
{
	struct interface *ifp;
	struct stream *s;

	s = client->ibuf;
	ifp = zebra_interface_state_read(s); /* it updates iflist */
	if (ifp == NULL)
		return 0;

	debugf(NHRP_DEBUG_IF, "if-delete: %s", ifp->name);
	ifp->ifindex = IFINDEX_INTERNAL;
	nhrp_interface_update(ifp);
	/* if_delete(ifp); */
	return 0;
}

int nhrp_interface_up(int cmd, struct zclient *client, zebra_size_t length)
{
	struct interface *ifp;

	ifp = zebra_interface_state_read(client->ibuf); /* it updates iflist */
	if (ifp == NULL)
		return 0;

	debugf(NHRP_DEBUG_IF, "if-up: %s", ifp->name);
	nhrp_interface_update(ifp);
	return 0;
}

int nhrp_interface_down(int cmd, struct zclient *client, zebra_size_t length)
{
	struct interface *ifp;

	ifp = zebra_interface_state_read(client->ibuf); /* it updates iflist */
	if (ifp == NULL)
		return 0;

	debugf(NHRP_DEBUG_IF, "if-down: %s", ifp->name);
	nhrp_interface_update(ifp);
	return 0;
}

int nhrp_interface_address_add(int cmd, struct zclient *client, zebra_size_t length)
{
	struct connected *ifc;
	char buf[PREFIX_STRLEN];

	ifc = zebra_interface_address_read(ZEBRA_INTERFACE_ADDRESS_ADD, client->ibuf);
	if (ifc == NULL)
		return 0;

	debugf(NHRP_DEBUG_IF, "if-addr-add: %s: %s",
		ifc->ifp->name,
		prefix2str(ifc->address, buf, sizeof buf));

	nhrp_interface_update_address(ifc->ifp, family2afi(PREFIX_FAMILY(ifc->address)));

	return 0;
}

int nhrp_interface_address_delete(int cmd, struct zclient *client, zebra_size_t length)
{
	struct connected *ifc;
	char buf[PREFIX_STRLEN];

	ifc = zebra_interface_address_read(ZEBRA_INTERFACE_ADDRESS_DELETE, client->ibuf);
	if (ifc == NULL)
		return 0;

	debugf(NHRP_DEBUG_IF, "if-addr-del: %s: %s",
		ifc->ifp->name,
		prefix2str(ifc->address, buf, sizeof buf));

	nhrp_interface_update_address(ifc->ifp, family2afi(PREFIX_FAMILY(ifc->address)));

	return 0;
}

void nhrp_interface_notify_add(struct interface *ifp, struct notifier_block *n, notifier_fn_t fn)
{
	struct nhrp_interface *nifp = ifp->info;
	notifier_add(n, &nifp->notifier_list, fn);
}

void nhrp_interface_notify_del(struct interface *ifp, struct notifier_block *n)
{
	notifier_del(n);
}

void nhrp_interface_set_protection(struct interface *ifp, const char *profile, const char *fallback_profile)
{
	struct nhrp_interface *nifp = ifp->info;

	if (nifp->ipsec_profile) free(nifp->ipsec_profile);
	nifp->ipsec_profile = profile ? strdup(profile) : NULL;

	if (nifp->ipsec_fallback_profile) free(nifp->ipsec_fallback_profile);
	nifp->ipsec_fallback_profile = fallback_profile ? strdup(fallback_profile) : NULL;
}