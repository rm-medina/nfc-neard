/*
 *
 *  neard - Near Field Communication manager
 *
 *  Copyright 2007, 2008	Johannes Berg <johannes@sipsolutions.net>
 *  Copyright (C) 2011 Instituto Nokia de Tecnologia
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>

#include "near.h"

#ifdef NEED_LIBNL_COMPAT
#define nl_sock nl_handle

static inline struct nl_handle *nl_socket_alloc(void)
{
	return nl_handle_alloc();
}

static inline void nl_socket_free(struct nl_sock *h)
{
	nl_handle_destroy(h);
}

static inline int __genl_ctrl_alloc_cache(struct nl_sock *h, struct nl_cache **cache)
{
	struct nl_cache *tmp = genl_ctrl_alloc_cache(h);
	if (!tmp)
		return -ENOMEM;
	*cache = tmp;
	return 0;
}
#define genl_ctrl_alloc_cache __genl_ctrl_alloc_cache

#define NLE_MISSING_ATTR	14

static inline void __nl_perror(int error, const char *s)
{
	nl_perror(s);
}
#define nl_perror __nl_perror
#endif

struct nlnfc_state {
	struct nl_sock *nl_sock;
	struct nl_cache *nl_cache;
	struct genl_family *nlnfc;
	int mcid;
};

static struct nlnfc_state *nfc_state;
static GIOChannel *netlink_channel = NULL;

static int error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err,
			 void *arg)
{
	int *ret = arg;

	DBG("");

	*ret = err->error;

	return NL_STOP;
}

static int finish_handler(struct nl_msg *msg, void *arg)
{
	int *ret = arg;

	DBG("");

	*ret = 1;

	return NL_SKIP;
}

static int ack_handler(struct nl_msg *msg, void *arg)
{
	int *ret = arg;

	DBG("");

	*ret = 1;

	return NL_STOP;
}

static int nl_send_msg(struct nl_sock *sock, struct nl_msg *msg,
			int (*rx_handler)(struct nl_msg *, void *),
			void *data)
{
	struct nl_cb *cb;
	int err, done;

	DBG("");

	cb = nl_cb_alloc(NL_CB_DEFAULT);
	if (cb == NULL)
		return -ENOMEM;

	err = nl_send_auto_complete(sock, msg);
	if (err < 0) {
		nl_cb_put(cb);
		near_error("%s", strerror(err));

		return err;
	}

	err = done = 0;

	nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &done);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &done);

	if (rx_handler != NULL)
		nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, rx_handler, data);

	while (err == 0 && done == 0)
		nl_recvmsgs(sock, cb);

	nl_cb_put(cb);

	return err;
}


static int get_devices_handler(struct nl_msg *n, void *arg)
{
	struct nlmsghdr *nlh = nlmsg_hdr(n);
	struct nlattr *attrs[NFC_ATTR_MAX + 1];
	char *name;
	uint32_t idx, protocols;
	near_bool_t powered;

	DBG("");

	genlmsg_parse(nlh, 0, attrs, NFC_ATTR_MAX, NULL);

	if (attrs[NFC_ATTR_DEVICE_INDEX] == NULL ||
	    attrs[NFC_ATTR_DEVICE_NAME] == NULL ||
	    attrs[NFC_ATTR_PROTOCOLS] == NULL) {
		nl_perror(NLE_MISSING_ATTR, "NFC_CMD_GET_DEVICE");
		return NL_STOP;
	}


	idx = nla_get_u32(attrs[NFC_ATTR_DEVICE_INDEX]);
	name = nla_get_string(attrs[NFC_ATTR_DEVICE_NAME]);
	protocols = nla_get_u32(attrs[NFC_ATTR_PROTOCOLS]);

	if (attrs[NFC_ATTR_DEVICE_POWERED] == NULL)
		powered = FALSE;
	else
		powered = nla_get_u8(attrs[NFC_ATTR_DEVICE_POWERED]);

	__near_manager_adapter_add(idx, name, protocols, powered);

	return NL_SKIP;
}

int __near_netlink_get_adapters(void)
{
	struct nl_msg *msg;
	void *hdr;
	int err, family;

	DBG("");

	if (nfc_state == NULL || nfc_state->nlnfc == NULL)
		return -ENODEV;

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	family = genl_family_get_id(nfc_state->nlnfc);

	hdr = genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0,
			  NLM_F_DUMP, NFC_CMD_GET_DEVICE, NFC_GENL_VERSION);
	if (hdr == NULL) {
		err = -EINVAL;
		goto out;
	}

	err = nl_send_msg(nfc_state->nl_sock, msg, get_devices_handler, NULL);

out:
	nlmsg_free(msg);

	return err;
}

int __near_netlink_start_poll(int idx,
				uint32_t im_protocols, uint32_t tm_protocols)
{
	struct nl_msg *msg;
	void *hdr;
	int family, err = 0;

	DBG("IM protos 0x%x TM protos 0x%x", im_protocols, tm_protocols);

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	family = genl_family_get_id(nfc_state->nlnfc);

	hdr = genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0,
			NLM_F_REQUEST, NFC_CMD_START_POLL, NFC_GENL_VERSION);
	if (hdr == NULL) {
		err = -EINVAL;
		goto nla_put_failure;
	}

	NLA_PUT_U32(msg, NFC_ATTR_DEVICE_INDEX, idx);
	if (im_protocols != 0)
		NLA_PUT_U32(msg, NFC_ATTR_IM_PROTOCOLS, im_protocols);
	if (tm_protocols != 0)
		NLA_PUT_U32(msg, NFC_ATTR_TM_PROTOCOLS, tm_protocols);

	err = nl_send_msg(nfc_state->nl_sock, msg, NULL, NULL);

nla_put_failure:
	nlmsg_free(msg);

	return err;
}


int __near_netlink_stop_poll(int idx)
{
	struct nl_msg *msg;
	void *hdr;
	int family, err = 0;

	DBG("");

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	family = genl_family_get_id(nfc_state->nlnfc);

	hdr = genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0,
			NLM_F_REQUEST, NFC_CMD_STOP_POLL, NFC_GENL_VERSION);
	if (hdr == NULL) {
		err = -EINVAL;
		goto nla_put_failure;
	}

	NLA_PUT_U32(msg, NFC_ATTR_DEVICE_INDEX, idx);

	err = nl_send_msg(nfc_state->nl_sock, msg, NULL, NULL);

nla_put_failure:
	nlmsg_free(msg);

	return err;
}

int __near_netlink_dep_link_up(uint32_t idx, uint32_t target_idx,
				uint8_t comm_mode, uint8_t rf_mode)
{
	struct nl_msg *msg;
	void *hdr;
	int family, err = 0;

	DBG("");

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	family = genl_family_get_id(nfc_state->nlnfc);

	hdr = genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0,
			NLM_F_REQUEST, NFC_CMD_DEP_LINK_UP, NFC_GENL_VERSION);
	if (hdr == NULL) {
		err = -EINVAL;
		goto nla_put_failure;
	}

	NLA_PUT_U32(msg, NFC_ATTR_DEVICE_INDEX, idx);
	NLA_PUT_U32(msg, NFC_ATTR_TARGET_INDEX, target_idx);
	NLA_PUT_U8(msg, NFC_ATTR_COMM_MODE, comm_mode);
	NLA_PUT_U8(msg, NFC_ATTR_RF_MODE, rf_mode);

	err = nl_send_msg(nfc_state->nl_sock, msg, NULL, NULL);

nla_put_failure:
	nlmsg_free(msg);

	return err;
}

int __near_netlink_dep_link_down(uint32_t idx)
{
	struct nl_msg *msg;
	void *hdr;
	int family, err = 0;

	DBG("");

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	family = genl_family_get_id(nfc_state->nlnfc);

	hdr = genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0,
			NLM_F_REQUEST, NFC_CMD_DEP_LINK_DOWN, NFC_GENL_VERSION);
	if (hdr == NULL) {
		err = -EINVAL;
		goto nla_put_failure;
	}

	NLA_PUT_U32(msg, NFC_ATTR_DEVICE_INDEX, idx);

	err = nl_send_msg(nfc_state->nl_sock, msg, NULL, NULL);

nla_put_failure:
	nlmsg_free(msg);

	return err;
}

int __near_netlink_adapter_enable(int idx, near_bool_t enable)
{
	struct nl_msg *msg;
	void *hdr;
	int family, err = 0;
	uint8_t cmd;

	DBG("");

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	family = genl_family_get_id(nfc_state->nlnfc);

	if (enable == TRUE)
		cmd = NFC_CMD_DEV_UP;
	else
		cmd = NFC_CMD_DEV_DOWN;

	hdr = genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0,
			NLM_F_REQUEST, cmd, NFC_GENL_VERSION);
	if (hdr == NULL) {
		err = -EINVAL;
		goto nla_put_failure;
	}

	NLA_PUT_U32(msg, NFC_ATTR_DEVICE_INDEX, idx);

	err = nl_send_msg(nfc_state->nl_sock, msg, NULL, NULL);

nla_put_failure:
	nlmsg_free(msg);

	return err;
}


static int no_seq_check(struct nl_msg *n, void *arg)
{
	DBG("");

	return NL_OK;
}

static int nfc_netlink_event_adapter(struct genlmsghdr *gnlh, near_bool_t add)
{
	struct nlattr *attrs[NFC_ATTR_MAX + 1];
	uint32_t idx;

	DBG("");

	nla_parse(attrs, NFC_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);
	if (attrs[NFC_ATTR_DEVICE_INDEX] == NULL) {
		near_error("Missing device index");
		return -ENODEV;
	}

	idx = nla_get_u32(attrs[NFC_ATTR_DEVICE_INDEX]);

	if (add == TRUE &&
		(attrs[NFC_ATTR_DEVICE_NAME] == NULL ||
			attrs[NFC_ATTR_PROTOCOLS] == NULL)) {
		near_error("Missing attributes");
		return -EINVAL;
	}

	if (add == TRUE) {
		char *name;
		uint32_t protocols;
		near_bool_t powered;

		name = nla_get_string(attrs[NFC_ATTR_DEVICE_NAME]);
		protocols = nla_get_u32(attrs[NFC_ATTR_PROTOCOLS]);
		if (attrs[NFC_ATTR_DEVICE_POWERED] == NULL)
			powered = FALSE;
		else
			powered = nla_get_u8(attrs[NFC_ATTR_DEVICE_POWERED]);

		return __near_manager_adapter_add(idx, name,
						protocols, powered);
	} else {
		__near_manager_adapter_remove(idx);
	}

	return 0;
}

static int get_targets_handler(struct nl_msg *n, void *arg)
{
	struct nlmsghdr *nlh = nlmsg_hdr(n);
	struct nlattr *attrs[NFC_ATTR_MAX + 1];
	uint32_t adapter_idx, target_idx, protocols;
	uint16_t sens_res = 0;
	uint8_t sel_res = 0;
	uint8_t nfcid[NFC_MAX_NFCID1_LEN], nfcid_len;

	DBG("");

	genlmsg_parse(nlh, 0, attrs, NFC_ATTR_MAX, NULL);

	adapter_idx = *((uint32_t *)arg);
	target_idx = nla_get_u32(attrs[NFC_ATTR_TARGET_INDEX]);
	protocols = nla_get_u32(attrs[NFC_ATTR_PROTOCOLS]);

	if (attrs[NFC_ATTR_TARGET_SENS_RES] != NULL)
		sens_res =
			nla_get_u16(attrs[NFC_ATTR_TARGET_SENS_RES]);

	if (attrs[NFC_ATTR_TARGET_SEL_RES] != NULL)
		sel_res =
			nla_get_u16(attrs[NFC_ATTR_TARGET_SEL_RES]);

	if (attrs[NFC_ATTR_TARGET_NFCID1] != NULL) {
		nfcid_len = nla_len(attrs[NFC_ATTR_TARGET_NFCID1]);
		if (nfcid_len <= NFC_MAX_NFCID1_LEN)
			memcpy(nfcid, nla_data(attrs[NFC_ATTR_TARGET_NFCID1]),
								nfcid_len);
	} else {
		nfcid_len = 0;
	}

	DBG("target idx %d proto 0x%x sens_res 0x%x sel_res 0x%x NFCID len %d",
	    target_idx, protocols, sens_res, sel_res, nfcid_len);

	__near_adapter_add_target(adapter_idx, target_idx, protocols,
				  sens_res, sel_res, nfcid, nfcid_len);

	return 0;
}

static int nfc_netlink_event_targets_found(struct genlmsghdr *gnlh)
{
	struct nlattr *attr[NFC_ATTR_MAX + 1];
	struct nl_msg *msg;
	void *hdr;
	int err, family;
	uint32_t adapter_idx;

	DBG("");

	nla_parse(attr, NFC_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
			genlmsg_attrlen(gnlh, 0), NULL);
	if (attr[NFC_ATTR_DEVICE_INDEX] == NULL)
		return -ENODEV;

	adapter_idx = nla_get_u32(attr[NFC_ATTR_DEVICE_INDEX]);

	DBG("adapter %d", adapter_idx);

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	family = genl_family_get_id(nfc_state->nlnfc);

	hdr = genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0,
			NLM_F_DUMP, NFC_CMD_GET_TARGET, NFC_GENL_VERSION);
	if (hdr == NULL) {
		err = -EINVAL;
		goto nla_put_failure;
	}

	NLA_PUT_U32(msg, NFC_ATTR_DEVICE_INDEX, adapter_idx);

	err = nl_send_msg(nfc_state->nl_sock, msg,
				get_targets_handler, &adapter_idx);

nla_put_failure:
	nlmsg_free(msg);

	return err;
}

static int nfc_netlink_event_target_lost(struct genlmsghdr *gnlh)
{
	struct nlattr *attr[NFC_ATTR_MAX + 1];
	uint32_t adapter_idx, target_idx;

	DBG("");

	nla_parse(attr, NFC_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
			genlmsg_attrlen(gnlh, 0), NULL);

	if (attr[NFC_ATTR_DEVICE_INDEX] == NULL)
		return -ENODEV;

	if (attr[NFC_ATTR_TARGET_INDEX] == NULL)
		return -ENODEV;

	adapter_idx = nla_get_u32(attr[NFC_ATTR_DEVICE_INDEX]);
	target_idx = nla_get_u32(attr[NFC_ATTR_TARGET_INDEX]);

	DBG("adapter %d target %d", adapter_idx, target_idx);

	return __near_adapter_remove_target(adapter_idx, target_idx);
}

static int nfc_netlink_event_dep_up(struct genlmsghdr *gnlh)
{
	struct nlattr *attrs[NFC_ATTR_MAX + 1];
	uint32_t idx, target_idx = 0;
	uint8_t rf_mode;

	DBG("");

	nla_parse(attrs, NFC_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);
	if (attrs[NFC_ATTR_DEVICE_INDEX] == NULL) {
		near_error("Missing device index");
		return -ENODEV;
	}

	if (attrs[NFC_ATTR_COMM_MODE] == NULL ||
			attrs[NFC_ATTR_RF_MODE] == NULL) {
		near_error("Missing rf or comm modes");
		return -ENODEV;
	}

	idx = nla_get_u32(attrs[NFC_ATTR_DEVICE_INDEX]);
	rf_mode = nla_get_u8(attrs[NFC_ATTR_RF_MODE]);

	if (rf_mode == NFC_RF_INITIATOR) {
		if (attrs[NFC_ATTR_TARGET_INDEX] == NULL) {
			near_error("Missing target index");
			return -ENODEV;
		};

		target_idx = nla_get_u32(attrs[NFC_ATTR_TARGET_INDEX]);

		DBG("%d %d", idx, target_idx);

		return __near_adapter_set_dep_state(idx, TRUE);
	} else {
		return -EOPNOTSUPP;
	}
}

static int nfc_netlink_event_dep_down(struct genlmsghdr *gnlh)
{
	struct nlattr *attrs[NFC_ATTR_MAX + 1];
	uint32_t idx;

	DBG("");

	nla_parse(attrs, NFC_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);
	if (attrs[NFC_ATTR_DEVICE_INDEX] == NULL) {
		near_error("Missing device index");
		return -ENODEV;
	}

	idx = nla_get_u32(attrs[NFC_ATTR_DEVICE_INDEX]);

	__near_adapter_set_dep_state(idx, FALSE);

	return 0;
}

static int nfc_netlink_event_tm_activated(struct genlmsghdr *gnlh)
{
	struct nlattr *attrs[NFC_ATTR_MAX + 1];
	uint32_t idx;

	DBG("");

	nla_parse(attrs, NFC_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);
	if (attrs[NFC_ATTR_DEVICE_INDEX] == NULL) {
		near_error("Missing device index");
		return -ENODEV;
	}

	idx = nla_get_u32(attrs[NFC_ATTR_DEVICE_INDEX]);

	DBG("%d", idx);

	return __near_adapter_add_device(idx, NULL, 0);
}

static int nfc_netlink_event_tm_deactivated(struct genlmsghdr *gnlh)
{
	struct nlattr *attrs[NFC_ATTR_MAX + 1];
	uint32_t idx;

	DBG("");

	nla_parse(attrs, NFC_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);
	if (attrs[NFC_ATTR_DEVICE_INDEX] == NULL) {
		near_error("Missing device index");
		return -ENODEV;
	}

	idx = nla_get_u32(attrs[NFC_ATTR_DEVICE_INDEX]);

	DBG("%d", idx);

	return __near_adapter_remove_device(idx);
}

static int nfc_netlink_event(struct nl_msg *n, void *arg)
{
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(n));

	DBG("event 0x%x", gnlh->cmd);

	switch (gnlh->cmd) {
	case NFC_EVENT_TARGETS_FOUND:
		DBG("Targets found");
		nfc_netlink_event_targets_found(gnlh);
		break;
	case NFC_EVENT_TARGET_LOST:
		DBG("Target lost");
		nfc_netlink_event_target_lost(gnlh);
		break;
	case NFC_EVENT_DEVICE_ADDED:
		DBG("Adapter added");
		nfc_netlink_event_adapter(gnlh, TRUE);

		break;
	case NFC_EVENT_DEVICE_REMOVED:
		DBG("Adapter removed");
		nfc_netlink_event_adapter(gnlh, FALSE);

		break;
	case NFC_CMD_DEP_LINK_UP:
		DBG("DEP link is up");
		nfc_netlink_event_dep_up(gnlh);

		break;
	case NFC_CMD_DEP_LINK_DOWN:
		DBG("DEP link is down");
		nfc_netlink_event_dep_down(gnlh);

		break;
	case NFC_EVENT_TM_ACTIVATED:
		DBG("Target mode activated");
		nfc_netlink_event_tm_activated(gnlh);

		break;
	case NFC_EVENT_TM_DEACTIVATED:
		DBG("Target mode deactivated");
		nfc_netlink_event_tm_deactivated(gnlh);

		break;
	}

	return NL_SKIP;
}

static gboolean __nfc_netlink_event(GIOChannel *channel,
				GIOCondition cond, gpointer data)
{
	struct nl_cb *cb;
	struct nlnfc_state *state = data;

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR))
		return FALSE;

	cb = nl_cb_alloc(NL_CB_VERBOSE);
	if (cb == NULL)
		return TRUE;

	nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check, NULL);
	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, nfc_netlink_event, data);

	nl_recvmsgs(state->nl_sock, cb);

	nl_cb_put(cb);

	return TRUE;
}

static int nfc_event_listener(struct nlnfc_state *state)
{
	int sock;

	sock = nl_socket_get_fd(state->nl_sock);
	netlink_channel = g_io_channel_unix_new(sock);
	g_io_channel_set_close_on_unref(netlink_channel, TRUE);

	g_io_channel_set_encoding(netlink_channel, NULL, NULL);
	g_io_channel_set_buffered(netlink_channel, FALSE);

	g_io_add_watch(netlink_channel,
				G_IO_IN | G_IO_NVAL | G_IO_HUP | G_IO_ERR,
						__nfc_netlink_event, state);

	return 0;
}

struct handler_args {
	const char *group;
	int id;
};

static int family_handler(struct nl_msg *msg, void *arg)
{
	struct handler_args *grp = arg;
	struct nlattr *tb[CTRL_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *mcgrp;
	int rem_mcgrp;

	DBG("");

	nla_parse(tb, CTRL_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);

        if (!tb[CTRL_ATTR_MCAST_GROUPS])
		return NL_SKIP;

	nla_for_each_nested(mcgrp, tb[CTRL_ATTR_MCAST_GROUPS], rem_mcgrp) {
		struct nlattr *tb_mcgrp[CTRL_ATTR_MCAST_GRP_MAX + 1];

		nla_parse(tb_mcgrp, CTRL_ATTR_MCAST_GRP_MAX,
			  nla_data(mcgrp), nla_len(mcgrp), NULL);

		if (!tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME] ||
		    !tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID])
			continue;
		if (strncmp(nla_data(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME]),
			    grp->group, nla_len(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME])))
			continue;
		grp->id = nla_get_u32(tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID]);
		break;
	}
	
	return NL_SKIP;
}

static int nl_get_multicast_id(struct nl_sock *sock, const char *family,
				const char *group)
{
	struct nl_msg *msg;
	int err = 0, ctrlid;
	struct handler_args grp = {
		.group = group,
		.id = -ENOENT,
	};

	DBG("");

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	ctrlid = genl_ctrl_resolve(sock, "nlctrl");

        genlmsg_put(msg, 0, 0, ctrlid, 0,
		    0, CTRL_CMD_GETFAMILY, 0);

	NLA_PUT_STRING(msg, CTRL_ATTR_FAMILY_NAME, family);

	err = nl_send_msg(sock, msg, family_handler, &grp);
	if (err)
		goto nla_put_failure;

	DBG("multicast id %d", grp.id);

	err = grp.id;

nla_put_failure:
	nlmsg_free(msg);

	return err;
}

int __near_netlink_init(void)
{
	int err;

	DBG("");

	nfc_state = g_try_malloc0(sizeof(struct nlnfc_state));
	if (nfc_state == NULL)
		return -ENOMEM;

	nfc_state->nl_sock = nl_socket_alloc();
	if (nfc_state->nl_sock == NULL) {
		near_error("Failed to allocate NFC netlink socket");
		err = -ENOMEM;
		goto state_free;
	}

	if (genl_connect(nfc_state->nl_sock)) {
		near_error("Failed to connect to generic netlink");
		err = -ENOLINK;
		goto handle_destroy;
	}

	if (genl_ctrl_alloc_cache(nfc_state->nl_sock, &nfc_state->nl_cache)) {
		near_error("Failed to allocate generic netlink cache");
		err = -ENOMEM;
		goto handle_destroy;
	}

	nfc_state->nlnfc = genl_ctrl_search_by_name(nfc_state->nl_cache, "nfc");
	if (nfc_state->nlnfc == NULL) {
		near_error("nfc not found");
		err = -ENOENT;
		goto cache_free;
	}

	nfc_state->mcid = nl_get_multicast_id(nfc_state->nl_sock, NFC_GENL_NAME,
						NFC_GENL_MCAST_EVENT_NAME);
	if (nfc_state->mcid <= 0) {
		near_error("Wrong mcast id %d", nfc_state->mcid);
		err = nfc_state->mcid;
		goto family_free;
	}

	err = nl_socket_add_membership(nfc_state->nl_sock, nfc_state->mcid);
	if (err) {
		near_error("Error adding nl socket to membership");
		goto family_free;
	}

	return nfc_event_listener(nfc_state);

family_free:
	genl_family_put(nfc_state->nlnfc);
cache_free:
	nl_cache_free(nfc_state->nl_cache);
handle_destroy:
	nl_socket_free(nfc_state->nl_sock);
state_free:
	g_free(nfc_state);

	nfc_state = NULL;

	near_error("netlink init failed");

	return err;
}

void __near_netlink_cleanup(void)
{
	if (netlink_channel != NULL) {
		g_io_channel_shutdown(netlink_channel, TRUE, NULL);
		g_io_channel_unref(netlink_channel);

		netlink_channel = NULL;
	}

	if (nfc_state == NULL)
		return;

	genl_family_put(nfc_state->nlnfc);
	nl_cache_free(nfc_state->nl_cache);
	nl_socket_free(nfc_state->nl_sock);

	g_free(nfc_state);

	DBG("");
}

