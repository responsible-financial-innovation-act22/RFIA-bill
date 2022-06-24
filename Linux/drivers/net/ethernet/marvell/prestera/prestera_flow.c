// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2020 Marvell International Ltd. All rights reserved */

#include <linux/kernel.h>
#include <linux/list.h>

#include "prestera.h"
#include "prestera_acl.h"
#include "prestera_flow.h"
#include "prestera_span.h"
#include "prestera_flower.h"

static LIST_HEAD(prestera_block_cb_list);

static int prestera_flow_block_mall_cb(struct prestera_flow_block *block,
				       struct tc_cls_matchall_offload *f)
{
	switch (f->command) {
	case TC_CLSMATCHALL_REPLACE:
		return prestera_span_replace(block, f);
	case TC_CLSMATCHALL_DESTROY:
		prestera_span_destroy(block);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int prestera_flow_block_flower_cb(struct prestera_flow_block *block,
					 struct flow_cls_offload *f)
{
	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return prestera_flower_replace(block, f);
	case FLOW_CLS_DESTROY:
		prestera_flower_destroy(block, f);
		return 0;
	case FLOW_CLS_STATS:
		return prestera_flower_stats(block, f);
	case FLOW_CLS_TMPLT_CREATE:
		return prestera_flower_tmplt_create(block, f);
	case FLOW_CLS_TMPLT_DESTROY:
		prestera_flower_tmplt_destroy(block, f);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int prestera_flow_block_cb(enum tc_setup_type type,
				  void *type_data, void *cb_priv)
{
	struct prestera_flow_block *block = cb_priv;

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return prestera_flow_block_flower_cb(block, type_data);
	case TC_SETUP_CLSMATCHALL:
		return prestera_flow_block_mall_cb(block, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

static void prestera_flow_block_destroy(void *cb_priv)
{
	struct prestera_flow_block *block = cb_priv;

	prestera_flower_template_cleanup(block);

	WARN_ON(!list_empty(&block->template_list));
	WARN_ON(!list_empty(&block->binding_list));

	kfree(block);
}

static struct prestera_flow_block *
prestera_flow_block_create(struct prestera_switch *sw, struct net *net)
{
	struct prestera_flow_block *block;

	block = kzalloc(sizeof(*block), GFP_KERNEL);
	if (!block)
		return NULL;

	INIT_LIST_HEAD(&block->binding_list);
	INIT_LIST_HEAD(&block->template_list);
	block->net = net;
	block->sw = sw;

	return block;
}

static void prestera_flow_block_release(void *cb_priv)
{
	struct prestera_flow_block *block = cb_priv;

	prestera_flow_block_destroy(block);
}

static bool
prestera_flow_block_is_bound(const struct prestera_flow_block *block)
{
	return block->ruleset_zero;
}

static struct prestera_flow_block_binding *
prestera_flow_block_lookup(struct prestera_flow_block *block,
			   struct prestera_port *port)
{
	struct prestera_flow_block_binding *binding;

	list_for_each_entry(binding, &block->binding_list, list)
		if (binding->port == port)
			return binding;

	return NULL;
}

static int prestera_flow_block_bind(struct prestera_flow_block *block,
				    struct prestera_port *port)
{
	struct prestera_flow_block_binding *binding;
	int err;

	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding)
		return -ENOMEM;

	binding->span_id = PRESTERA_SPAN_INVALID_ID;
	binding->port = port;

	if (prestera_flow_block_is_bound(block)) {
		err = prestera_acl_ruleset_bind(block->ruleset_zero, port);
		if (err)
			goto err_ruleset_bind;
	}

	list_add(&binding->list, &block->binding_list);
	return 0;

err_ruleset_bind:
	kfree(binding);
	return err;
}

static int prestera_flow_block_unbind(struct prestera_flow_block *block,
				      struct prestera_port *port)
{
	struct prestera_flow_block_binding *binding;

	binding = prestera_flow_block_lookup(block, port);
	if (!binding)
		return -ENOENT;

	list_del(&binding->list);

	if (prestera_flow_block_is_bound(block))
		prestera_acl_ruleset_unbind(block->ruleset_zero, port);

	kfree(binding);
	return 0;
}

static struct prestera_flow_block *
prestera_flow_block_get(struct prestera_switch *sw,
			struct flow_block_offload *f,
			bool *register_block)
{
	struct prestera_flow_block *block;
	struct flow_block_cb *block_cb;

	block_cb = flow_block_cb_lookup(f->block,
					prestera_flow_block_cb, sw);
	if (!block_cb) {
		block = prestera_flow_block_create(sw, f->net);
		if (!block)
			return ERR_PTR(-ENOMEM);

		block_cb = flow_block_cb_alloc(prestera_flow_block_cb,
					       sw, block,
					       prestera_flow_block_release);
		if (IS_ERR(block_cb)) {
			prestera_flow_block_destroy(block);
			return ERR_CAST(block_cb);
		}

		block->block_cb = block_cb;
		*register_block = true;
	} else {
		block = flow_block_cb_priv(block_cb);
		*register_block = false;
	}

	flow_block_cb_incref(block_cb);

	return block;
}

static void prestera_flow_block_put(struct prestera_flow_block *block)
{
	struct flow_block_cb *block_cb = block->block_cb;

	if (flow_block_cb_decref(block_cb))
		return;

	flow_block_cb_free(block_cb);
	prestera_flow_block_destroy(block);
}

static int prestera_setup_flow_block_bind(struct prestera_port *port,
					  struct flow_block_offload *f)
{
	struct prestera_switch *sw = port->sw;
	struct prestera_flow_block *block;
	struct flow_block_cb *block_cb;
	bool register_block;
	int err;

	block = prestera_flow_block_get(sw, f, &register_block);
	if (IS_ERR(block))
		return PTR_ERR(block);

	block_cb = block->block_cb;

	err = prestera_flow_block_bind(block, port);
	if (err)
		goto err_block_bind;

	if (register_block) {
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &prestera_block_cb_list);
	}

	port->flow_block = block;
	return 0;

err_block_bind:
	prestera_flow_block_put(block);

	return err;
}

static void prestera_setup_flow_block_unbind(struct prestera_port *port,
					     struct flow_block_offload *f)
{
	struct prestera_switch *sw = port->sw;
	struct prestera_flow_block *block;
	struct flow_block_cb *block_cb;
	int err;

	block_cb = flow_block_cb_lookup(f->block, prestera_flow_block_cb, sw);
	if (!block_cb)
		return;

	block = flow_block_cb_priv(block_cb);

	prestera_span_destroy(block);

	err = prestera_flow_block_unbind(block, port);
	if (err)
		goto error;

	if (!flow_block_cb_decref(block_cb)) {
		flow_block_cb_remove(block_cb, f);
		list_del(&block_cb->driver_list);
	}
error:
	port->flow_block = NULL;
}

int prestera_flow_block_setup(struct prestera_port *port,
			      struct flow_block_offload *f)
{
	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &prestera_block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		return prestera_setup_flow_block_bind(port, f);
	case FLOW_BLOCK_UNBIND:
		prestera_setup_flow_block_unbind(port, f);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
