// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>
#include <linux/of_address.h>
#include <linux/iommu.h>
#include "ahb.h"
#include "debug.h"
#include "hif.h"
#include <linux/remoteproc.h>
#include "pcic.h"

static const struct of_device_id ath11k_ahb_of_match[] = {
	/* TODO: Should we change the compatible string to something similar
	 * to one that ath10k uses?
	 */
	{ .compatible = "qcom,ipq8074-wifi",
	  .data = (void *)ATH11K_HW_IPQ8074,
	},
	{ .compatible = "qcom,ipq6018-wifi",
	  .data = (void *)ATH11K_HW_IPQ6018_HW10,
	},
	{ .compatible = "qcom,wcn6750-wifi",
	  .data = (void *)ATH11K_HW_WCN6750_HW10,
	},
	{ }
};

MODULE_DEVICE_TABLE(of, ath11k_ahb_of_match);

#define ATH11K_IRQ_CE0_OFFSET 4

static const char *irq_name[ATH11K_IRQ_NUM_MAX] = {
	"misc-pulse1",
	"misc-latch",
	"sw-exception",
	"watchdog",
	"ce0",
	"ce1",
	"ce2",
	"ce3",
	"ce4",
	"ce5",
	"ce6",
	"ce7",
	"ce8",
	"ce9",
	"ce10",
	"ce11",
	"host2wbm-desc-feed",
	"host2reo-re-injection",
	"host2reo-command",
	"host2rxdma-monitor-ring3",
	"host2rxdma-monitor-ring2",
	"host2rxdma-monitor-ring1",
	"reo2ost-exception",
	"wbm2host-rx-release",
	"reo2host-status",
	"reo2host-destination-ring4",
	"reo2host-destination-ring3",
	"reo2host-destination-ring2",
	"reo2host-destination-ring1",
	"rxdma2host-monitor-destination-mac3",
	"rxdma2host-monitor-destination-mac2",
	"rxdma2host-monitor-destination-mac1",
	"ppdu-end-interrupts-mac3",
	"ppdu-end-interrupts-mac2",
	"ppdu-end-interrupts-mac1",
	"rxdma2host-monitor-status-ring-mac3",
	"rxdma2host-monitor-status-ring-mac2",
	"rxdma2host-monitor-status-ring-mac1",
	"host2rxdma-host-buf-ring-mac3",
	"host2rxdma-host-buf-ring-mac2",
	"host2rxdma-host-buf-ring-mac1",
	"rxdma2host-destination-ring-mac3",
	"rxdma2host-destination-ring-mac2",
	"rxdma2host-destination-ring-mac1",
	"host2tcl-input-ring4",
	"host2tcl-input-ring3",
	"host2tcl-input-ring2",
	"host2tcl-input-ring1",
	"wbm2host-tx-completions-ring3",
	"wbm2host-tx-completions-ring2",
	"wbm2host-tx-completions-ring1",
	"tcl2host-status-ring",
};

/* enum ext_irq_num - irq numbers that can be used by external modules
 * like datapath
 */
enum ext_irq_num {
	host2wbm_desc_feed = 16,
	host2reo_re_injection,
	host2reo_command,
	host2rxdma_monitor_ring3,
	host2rxdma_monitor_ring2,
	host2rxdma_monitor_ring1,
	reo2host_exception,
	wbm2host_rx_release,
	reo2host_status,
	reo2host_destination_ring4,
	reo2host_destination_ring3,
	reo2host_destination_ring2,
	reo2host_destination_ring1,
	rxdma2host_monitor_destination_mac3,
	rxdma2host_monitor_destination_mac2,
	rxdma2host_monitor_destination_mac1,
	ppdu_end_interrupts_mac3,
	ppdu_end_interrupts_mac2,
	ppdu_end_interrupts_mac1,
	rxdma2host_monitor_status_ring_mac3,
	rxdma2host_monitor_status_ring_mac2,
	rxdma2host_monitor_status_ring_mac1,
	host2rxdma_host_buf_ring_mac3,
	host2rxdma_host_buf_ring_mac2,
	host2rxdma_host_buf_ring_mac1,
	rxdma2host_destination_ring_mac3,
	rxdma2host_destination_ring_mac2,
	rxdma2host_destination_ring_mac1,
	host2tcl_input_ring4,
	host2tcl_input_ring3,
	host2tcl_input_ring2,
	host2tcl_input_ring1,
	wbm2host_tx_completions_ring3,
	wbm2host_tx_completions_ring2,
	wbm2host_tx_completions_ring1,
	tcl2host_status_ring,
};

static int
ath11k_ahb_get_msi_irq_wcn6750(struct ath11k_base *ab, unsigned int vector)
{
	return ab->pci.msi.irqs[vector];
}

static const struct ath11k_pci_ops ath11k_ahb_pci_ops_wcn6750 = {
	.get_msi_irq = ath11k_ahb_get_msi_irq_wcn6750,
};

static inline u32 ath11k_ahb_read32(struct ath11k_base *ab, u32 offset)
{
	return ioread32(ab->mem + offset);
}

static inline void ath11k_ahb_write32(struct ath11k_base *ab, u32 offset, u32 value)
{
	iowrite32(value, ab->mem + offset);
}

static void ath11k_ahb_kill_tasklets(struct ath11k_base *ab)
{
	int i;

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		struct ath11k_ce_pipe *ce_pipe = &ab->ce.ce_pipe[i];

		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;

		tasklet_kill(&ce_pipe->intr_tq);
	}
}

static void ath11k_ahb_ext_grp_disable(struct ath11k_ext_irq_grp *irq_grp)
{
	int i;

	for (i = 0; i < irq_grp->num_irq; i++)
		disable_irq_nosync(irq_grp->ab->irq_num[irq_grp->irqs[i]]);
}

static void __ath11k_ahb_ext_irq_disable(struct ath11k_base *ab)
{
	int i;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &ab->ext_irq_grp[i];

		ath11k_ahb_ext_grp_disable(irq_grp);

		if (irq_grp->napi_enabled) {
			napi_synchronize(&irq_grp->napi);
			napi_disable(&irq_grp->napi);
			irq_grp->napi_enabled = false;
		}
	}
}

static void ath11k_ahb_ext_grp_enable(struct ath11k_ext_irq_grp *irq_grp)
{
	int i;

	for (i = 0; i < irq_grp->num_irq; i++)
		enable_irq(irq_grp->ab->irq_num[irq_grp->irqs[i]]);
}

static void ath11k_ahb_setbit32(struct ath11k_base *ab, u8 bit, u32 offset)
{
	u32 val;

	val = ath11k_ahb_read32(ab, offset);
	ath11k_ahb_write32(ab, offset, val | BIT(bit));
}

static void ath11k_ahb_clearbit32(struct ath11k_base *ab, u8 bit, u32 offset)
{
	u32 val;

	val = ath11k_ahb_read32(ab, offset);
	ath11k_ahb_write32(ab, offset, val & ~BIT(bit));
}

static void ath11k_ahb_ce_irq_enable(struct ath11k_base *ab, u16 ce_id)
{
	const struct ce_attr *ce_attr;

	ce_attr = &ab->hw_params.host_ce_config[ce_id];
	if (ce_attr->src_nentries)
		ath11k_ahb_setbit32(ab, ce_id, CE_HOST_IE_ADDRESS);

	if (ce_attr->dest_nentries) {
		ath11k_ahb_setbit32(ab, ce_id, CE_HOST_IE_2_ADDRESS);
		ath11k_ahb_setbit32(ab, ce_id + CE_HOST_IE_3_SHIFT,
				    CE_HOST_IE_3_ADDRESS);
	}
}

static void ath11k_ahb_ce_irq_disable(struct ath11k_base *ab, u16 ce_id)
{
	const struct ce_attr *ce_attr;

	ce_attr = &ab->hw_params.host_ce_config[ce_id];
	if (ce_attr->src_nentries)
		ath11k_ahb_clearbit32(ab, ce_id, CE_HOST_IE_ADDRESS);

	if (ce_attr->dest_nentries) {
		ath11k_ahb_clearbit32(ab, ce_id, CE_HOST_IE_2_ADDRESS);
		ath11k_ahb_clearbit32(ab, ce_id + CE_HOST_IE_3_SHIFT,
				      CE_HOST_IE_3_ADDRESS);
	}
}

static void ath11k_ahb_sync_ce_irqs(struct ath11k_base *ab)
{
	int i;
	int irq_idx;

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;

		irq_idx = ATH11K_IRQ_CE0_OFFSET + i;
		synchronize_irq(ab->irq_num[irq_idx]);
	}
}

static void ath11k_ahb_sync_ext_irqs(struct ath11k_base *ab)
{
	int i, j;
	int irq_idx;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &ab->ext_irq_grp[i];

		for (j = 0; j < irq_grp->num_irq; j++) {
			irq_idx = irq_grp->irqs[j];
			synchronize_irq(ab->irq_num[irq_idx]);
		}
	}
}

static void ath11k_ahb_ce_irqs_enable(struct ath11k_base *ab)
{
	int i;

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;
		ath11k_ahb_ce_irq_enable(ab, i);
	}
}

static void ath11k_ahb_ce_irqs_disable(struct ath11k_base *ab)
{
	int i;

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;
		ath11k_ahb_ce_irq_disable(ab, i);
	}
}

static int ath11k_ahb_start(struct ath11k_base *ab)
{
	ath11k_ahb_ce_irqs_enable(ab);
	ath11k_ce_rx_post_buf(ab);

	return 0;
}

static void ath11k_ahb_ext_irq_enable(struct ath11k_base *ab)
{
	int i;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &ab->ext_irq_grp[i];

		if (!irq_grp->napi_enabled) {
			napi_enable(&irq_grp->napi);
			irq_grp->napi_enabled = true;
		}
		ath11k_ahb_ext_grp_enable(irq_grp);
	}
}

static void ath11k_ahb_ext_irq_disable(struct ath11k_base *ab)
{
	__ath11k_ahb_ext_irq_disable(ab);
	ath11k_ahb_sync_ext_irqs(ab);
}

static void ath11k_ahb_stop(struct ath11k_base *ab)
{
	if (!test_bit(ATH11K_FLAG_CRASH_FLUSH, &ab->dev_flags))
		ath11k_ahb_ce_irqs_disable(ab);
	ath11k_ahb_sync_ce_irqs(ab);
	ath11k_ahb_kill_tasklets(ab);
	del_timer_sync(&ab->rx_replenish_retry);
	ath11k_ce_cleanup_pipes(ab);
}

static int ath11k_ahb_power_up(struct ath11k_base *ab)
{
	struct ath11k_ahb *ab_ahb = ath11k_ahb_priv(ab);
	int ret;

	ret = rproc_boot(ab_ahb->tgt_rproc);
	if (ret)
		ath11k_err(ab, "failed to boot the remote processor Q6\n");

	return ret;
}

static void ath11k_ahb_power_down(struct ath11k_base *ab)
{
	struct ath11k_ahb *ab_ahb = ath11k_ahb_priv(ab);

	rproc_shutdown(ab_ahb->tgt_rproc);
}

static int ath11k_ahb_fwreset_from_cold_boot(struct ath11k_base *ab)
{
	int timeout;

	if (ath11k_cold_boot_cal == 0 || ab->qmi.cal_done ||
	    ab->hw_params.cold_boot_calib == 0)
		return 0;

	ath11k_dbg(ab, ATH11K_DBG_AHB, "wait for cold boot done\n");
	timeout = wait_event_timeout(ab->qmi.cold_boot_waitq,
				     (ab->qmi.cal_done  == 1),
				     ATH11K_COLD_BOOT_FW_RESET_DELAY);
	if (timeout <= 0) {
		ath11k_cold_boot_cal = 0;
		ath11k_warn(ab, "Coldboot Calibration failed timed out\n");
	}

	/* reset the firmware */
	ath11k_ahb_power_down(ab);
	ath11k_ahb_power_up(ab);

	ath11k_dbg(ab, ATH11K_DBG_AHB, "exited from cold boot mode\n");
	return 0;
}

static void ath11k_ahb_init_qmi_ce_config(struct ath11k_base *ab)
{
	struct ath11k_qmi_ce_cfg *cfg = &ab->qmi.ce_cfg;

	cfg->tgt_ce_len = ab->hw_params.target_ce_count;
	cfg->tgt_ce = ab->hw_params.target_ce_config;
	cfg->svc_to_ce_map_len = ab->hw_params.svc_to_ce_map_len;
	cfg->svc_to_ce_map = ab->hw_params.svc_to_ce_map;
	ab->qmi.service_ins_id = ab->hw_params.qmi_service_ins_id;
}

static void ath11k_ahb_free_ext_irq(struct ath11k_base *ab)
{
	int i, j;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &ab->ext_irq_grp[i];

		for (j = 0; j < irq_grp->num_irq; j++)
			free_irq(ab->irq_num[irq_grp->irqs[j]], irq_grp);

		netif_napi_del(&irq_grp->napi);
	}
}

static void ath11k_ahb_free_irq(struct ath11k_base *ab)
{
	int irq_idx;
	int i;

	if (ab->hw_params.hybrid_bus_type)
		return ath11k_pcic_free_irq(ab);

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;
		irq_idx = ATH11K_IRQ_CE0_OFFSET + i;
		free_irq(ab->irq_num[irq_idx], &ab->ce.ce_pipe[i]);
	}

	ath11k_ahb_free_ext_irq(ab);
}

static void ath11k_ahb_ce_tasklet(struct tasklet_struct *t)
{
	struct ath11k_ce_pipe *ce_pipe = from_tasklet(ce_pipe, t, intr_tq);

	ath11k_ce_per_engine_service(ce_pipe->ab, ce_pipe->pipe_num);

	ath11k_ahb_ce_irq_enable(ce_pipe->ab, ce_pipe->pipe_num);
}

static irqreturn_t ath11k_ahb_ce_interrupt_handler(int irq, void *arg)
{
	struct ath11k_ce_pipe *ce_pipe = arg;

	/* last interrupt received for this CE */
	ce_pipe->timestamp = jiffies;

	ath11k_ahb_ce_irq_disable(ce_pipe->ab, ce_pipe->pipe_num);

	tasklet_schedule(&ce_pipe->intr_tq);

	return IRQ_HANDLED;
}

static int ath11k_ahb_ext_grp_napi_poll(struct napi_struct *napi, int budget)
{
	struct ath11k_ext_irq_grp *irq_grp = container_of(napi,
						struct ath11k_ext_irq_grp,
						napi);
	struct ath11k_base *ab = irq_grp->ab;
	int work_done;

	work_done = ath11k_dp_service_srng(ab, irq_grp, budget);
	if (work_done < budget) {
		napi_complete_done(napi, work_done);
		ath11k_ahb_ext_grp_enable(irq_grp);
	}

	if (work_done > budget)
		work_done = budget;

	return work_done;
}

static irqreturn_t ath11k_ahb_ext_interrupt_handler(int irq, void *arg)
{
	struct ath11k_ext_irq_grp *irq_grp = arg;

	/* last interrupt received for this group */
	irq_grp->timestamp = jiffies;

	ath11k_ahb_ext_grp_disable(irq_grp);

	napi_schedule(&irq_grp->napi);

	return IRQ_HANDLED;
}

static int ath11k_ahb_config_ext_irq(struct ath11k_base *ab)
{
	struct ath11k_hw_params *hw = &ab->hw_params;
	int i, j;
	int irq;
	int ret;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &ab->ext_irq_grp[i];
		u32 num_irq = 0;

		irq_grp->ab = ab;
		irq_grp->grp_id = i;
		init_dummy_netdev(&irq_grp->napi_ndev);
		netif_napi_add(&irq_grp->napi_ndev, &irq_grp->napi,
			       ath11k_ahb_ext_grp_napi_poll, NAPI_POLL_WEIGHT);

		for (j = 0; j < ATH11K_EXT_IRQ_NUM_MAX; j++) {
			if (ab->hw_params.ring_mask->tx[i] & BIT(j)) {
				irq_grp->irqs[num_irq++] =
					wbm2host_tx_completions_ring1 - j;
			}

			if (ab->hw_params.ring_mask->rx[i] & BIT(j)) {
				irq_grp->irqs[num_irq++] =
					reo2host_destination_ring1 - j;
			}

			if (ab->hw_params.ring_mask->rx_err[i] & BIT(j))
				irq_grp->irqs[num_irq++] = reo2host_exception;

			if (ab->hw_params.ring_mask->rx_wbm_rel[i] & BIT(j))
				irq_grp->irqs[num_irq++] = wbm2host_rx_release;

			if (ab->hw_params.ring_mask->reo_status[i] & BIT(j))
				irq_grp->irqs[num_irq++] = reo2host_status;

			if (j < ab->hw_params.max_radios) {
				if (ab->hw_params.ring_mask->rxdma2host[i] & BIT(j)) {
					irq_grp->irqs[num_irq++] =
						rxdma2host_destination_ring_mac1 -
						ath11k_hw_get_mac_from_pdev_id(hw, j);
				}

				if (ab->hw_params.ring_mask->host2rxdma[i] & BIT(j)) {
					irq_grp->irqs[num_irq++] =
						host2rxdma_host_buf_ring_mac1 -
						ath11k_hw_get_mac_from_pdev_id(hw, j);
				}

				if (ab->hw_params.ring_mask->rx_mon_status[i] & BIT(j)) {
					irq_grp->irqs[num_irq++] =
						ppdu_end_interrupts_mac1 -
						ath11k_hw_get_mac_from_pdev_id(hw, j);
					irq_grp->irqs[num_irq++] =
						rxdma2host_monitor_status_ring_mac1 -
						ath11k_hw_get_mac_from_pdev_id(hw, j);
				}
			}
		}
		irq_grp->num_irq = num_irq;

		for (j = 0; j < irq_grp->num_irq; j++) {
			int irq_idx = irq_grp->irqs[j];

			irq = platform_get_irq_byname(ab->pdev,
						      irq_name[irq_idx]);
			ab->irq_num[irq_idx] = irq;
			irq_set_status_flags(irq, IRQ_NOAUTOEN | IRQ_DISABLE_UNLAZY);
			ret = request_irq(irq, ath11k_ahb_ext_interrupt_handler,
					  IRQF_TRIGGER_RISING,
					  irq_name[irq_idx], irq_grp);
			if (ret) {
				ath11k_err(ab, "failed request_irq for %d\n",
					   irq);
			}
		}
	}

	return 0;
}

static int ath11k_ahb_config_irq(struct ath11k_base *ab)
{
	int irq, irq_idx, i;
	int ret;

	if (ab->hw_params.hybrid_bus_type)
		return ath11k_pcic_config_irq(ab);

	/* Configure CE irqs */
	for (i = 0; i < ab->hw_params.ce_count; i++) {
		struct ath11k_ce_pipe *ce_pipe = &ab->ce.ce_pipe[i];

		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;

		irq_idx = ATH11K_IRQ_CE0_OFFSET + i;

		tasklet_setup(&ce_pipe->intr_tq, ath11k_ahb_ce_tasklet);
		irq = platform_get_irq_byname(ab->pdev, irq_name[irq_idx]);
		ret = request_irq(irq, ath11k_ahb_ce_interrupt_handler,
				  IRQF_TRIGGER_RISING, irq_name[irq_idx],
				  ce_pipe);
		if (ret)
			return ret;

		ab->irq_num[irq_idx] = irq;
	}

	/* Configure external interrupts */
	ret = ath11k_ahb_config_ext_irq(ab);

	return ret;
}

static int ath11k_ahb_map_service_to_pipe(struct ath11k_base *ab, u16 service_id,
					  u8 *ul_pipe, u8 *dl_pipe)
{
	const struct service_to_pipe *entry;
	bool ul_set = false, dl_set = false;
	int i;

	for (i = 0; i < ab->hw_params.svc_to_ce_map_len; i++) {
		entry = &ab->hw_params.svc_to_ce_map[i];

		if (__le32_to_cpu(entry->service_id) != service_id)
			continue;

		switch (__le32_to_cpu(entry->pipedir)) {
		case PIPEDIR_NONE:
			break;
		case PIPEDIR_IN:
			WARN_ON(dl_set);
			*dl_pipe = __le32_to_cpu(entry->pipenum);
			dl_set = true;
			break;
		case PIPEDIR_OUT:
			WARN_ON(ul_set);
			*ul_pipe = __le32_to_cpu(entry->pipenum);
			ul_set = true;
			break;
		case PIPEDIR_INOUT:
			WARN_ON(dl_set);
			WARN_ON(ul_set);
			*dl_pipe = __le32_to_cpu(entry->pipenum);
			*ul_pipe = __le32_to_cpu(entry->pipenum);
			dl_set = true;
			ul_set = true;
			break;
		}
	}

	if (WARN_ON(!ul_set || !dl_set))
		return -ENOENT;

	return 0;
}

static const struct ath11k_hif_ops ath11k_ahb_hif_ops_ipq8074 = {
	.start = ath11k_ahb_start,
	.stop = ath11k_ahb_stop,
	.read32 = ath11k_ahb_read32,
	.write32 = ath11k_ahb_write32,
	.irq_enable = ath11k_ahb_ext_irq_enable,
	.irq_disable = ath11k_ahb_ext_irq_disable,
	.map_service_to_pipe = ath11k_ahb_map_service_to_pipe,
	.power_down = ath11k_ahb_power_down,
	.power_up = ath11k_ahb_power_up,
};

static const struct ath11k_hif_ops ath11k_ahb_hif_ops_wcn6750 = {
	.start = ath11k_pcic_start,
	.stop = ath11k_pcic_stop,
	.read32 = ath11k_pcic_read32,
	.write32 = ath11k_pcic_write32,
	.irq_enable = ath11k_pcic_ext_irq_enable,
	.irq_disable = ath11k_pcic_ext_irq_disable,
	.get_msi_address =  ath11k_pcic_get_msi_address,
	.get_user_msi_vector = ath11k_pcic_get_user_msi_assignment,
	.map_service_to_pipe = ath11k_pcic_map_service_to_pipe,
	.power_down = ath11k_ahb_power_down,
	.power_up = ath11k_ahb_power_up,
};

static int ath11k_core_get_rproc(struct ath11k_base *ab)
{
	struct ath11k_ahb *ab_ahb = ath11k_ahb_priv(ab);
	struct device *dev = ab->dev;
	struct rproc *prproc;
	phandle rproc_phandle;

	if (of_property_read_u32(dev->of_node, "qcom,rproc", &rproc_phandle)) {
		ath11k_err(ab, "failed to get q6_rproc handle\n");
		return -ENOENT;
	}

	prproc = rproc_get_by_phandle(rproc_phandle);
	if (!prproc) {
		ath11k_err(ab, "failed to get rproc\n");
		return -EINVAL;
	}
	ab_ahb->tgt_rproc = prproc;

	return 0;
}

static int ath11k_ahb_setup_msi_resources(struct ath11k_base *ab)
{
	struct platform_device *pdev = ab->pdev;
	phys_addr_t msi_addr_pa;
	dma_addr_t msi_addr_iova;
	struct resource *res;
	int int_prop;
	int ret;
	int i;

	ret = ath11k_pcic_init_msi_config(ab);
	if (ret) {
		ath11k_err(ab, "failed to init msi config: %d\n", ret);
		return ret;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ath11k_err(ab, "failed to fetch msi_addr\n");
		return -ENOENT;
	}

	msi_addr_pa = res->start;
	msi_addr_iova = dma_map_resource(ab->dev, msi_addr_pa, PAGE_SIZE,
					 DMA_FROM_DEVICE, 0);
	if (dma_mapping_error(ab->dev, msi_addr_iova))
		return -ENOMEM;

	ab->pci.msi.addr_lo = lower_32_bits(msi_addr_iova);
	ab->pci.msi.addr_hi = upper_32_bits(msi_addr_iova);

	ret = of_property_read_u32_index(ab->dev->of_node, "interrupts", 1, &int_prop);
	if (ret)
		return ret;

	ab->pci.msi.ep_base_data = int_prop + 32;

	for (i = 0; i < ab->pci.msi.config->total_vectors; i++) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ, i);
		if (!res)
			return -ENODEV;

		ab->pci.msi.irqs[i] = res->start;
	}

	set_bit(ATH11K_FLAG_MULTI_MSI_VECTORS, &ab->dev_flags);

	return 0;
}

static int ath11k_ahb_setup_resources(struct ath11k_base *ab)
{
	struct platform_device *pdev = ab->pdev;
	struct resource *mem_res;
	void __iomem *mem;

	if (ab->hw_params.hybrid_bus_type)
		return ath11k_ahb_setup_msi_resources(ab);

	mem = devm_platform_get_and_ioremap_resource(pdev, 0, &mem_res);
	if (IS_ERR(mem)) {
		dev_err(&pdev->dev, "ioremap error\n");
		return PTR_ERR(mem);
	}

	ab->mem = mem;
	ab->mem_len = resource_size(mem_res);

	return 0;
}

static int ath11k_ahb_setup_msa_resources(struct ath11k_base *ab)
{
	struct ath11k_ahb *ab_ahb = ath11k_ahb_priv(ab);
	struct device *dev = ab->dev;
	struct device_node *node;
	struct resource r;
	int ret;

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node)
		return -ENOENT;

	ret = of_address_to_resource(node, 0, &r);
	of_node_put(node);
	if (ret) {
		dev_err(dev, "failed to resolve msa fixed region\n");
		return ret;
	}

	ab_ahb->fw.msa_paddr = r.start;
	ab_ahb->fw.msa_size = resource_size(&r);

	node = of_parse_phandle(dev->of_node, "memory-region", 1);
	if (!node)
		return -ENOENT;

	ret = of_address_to_resource(node, 0, &r);
	of_node_put(node);
	if (ret) {
		dev_err(dev, "failed to resolve ce fixed region\n");
		return ret;
	}

	ab_ahb->fw.ce_paddr = r.start;
	ab_ahb->fw.ce_size = resource_size(&r);

	return 0;
}

static int ath11k_ahb_fw_resources_init(struct ath11k_base *ab)
{
	struct ath11k_ahb *ab_ahb = ath11k_ahb_priv(ab);
	struct device *host_dev = ab->dev;
	struct platform_device_info info = {0};
	struct iommu_domain *iommu_dom;
	struct platform_device *pdev;
	struct device_node *node;
	int ret;

	/* Chipsets not requiring MSA need not initialize
	 * MSA resources, return success in such cases.
	 */
	if (!ab->hw_params.fixed_fw_mem)
		return 0;

	ret = ath11k_ahb_setup_msa_resources(ab);
	if (ret) {
		ath11k_err(ab, "failed to setup msa resources\n");
		return ret;
	}

	node = of_get_child_by_name(host_dev->of_node, "wifi-firmware");
	if (!node) {
		ab_ahb->fw.use_tz = true;
		return 0;
	}

	info.fwnode = &node->fwnode;
	info.parent = host_dev;
	info.name = node->name;
	info.dma_mask = DMA_BIT_MASK(32);

	pdev = platform_device_register_full(&info);
	if (IS_ERR(pdev)) {
		of_node_put(node);
		return PTR_ERR(pdev);
	}

	ret = of_dma_configure(&pdev->dev, node, true);
	if (ret) {
		ath11k_err(ab, "dma configure fail: %d\n", ret);
		goto err_unregister;
	}

	ab_ahb->fw.dev = &pdev->dev;

	iommu_dom = iommu_domain_alloc(&platform_bus_type);
	if (!iommu_dom) {
		ath11k_err(ab, "failed to allocate iommu domain\n");
		ret = -ENOMEM;
		goto err_unregister;
	}

	ret = iommu_attach_device(iommu_dom, ab_ahb->fw.dev);
	if (ret) {
		ath11k_err(ab, "could not attach device: %d\n", ret);
		goto err_iommu_free;
	}

	ret = iommu_map(iommu_dom, ab_ahb->fw.msa_paddr,
			ab_ahb->fw.msa_paddr, ab_ahb->fw.msa_size,
			IOMMU_READ | IOMMU_WRITE);
	if (ret) {
		ath11k_err(ab, "failed to map firmware region: %d\n", ret);
		goto err_iommu_detach;
	}

	ret = iommu_map(iommu_dom, ab_ahb->fw.ce_paddr,
			ab_ahb->fw.ce_paddr, ab_ahb->fw.ce_size,
			IOMMU_READ | IOMMU_WRITE);
	if (ret) {
		ath11k_err(ab, "failed to map firmware CE region: %d\n", ret);
		goto err_iommu_unmap;
	}

	ab_ahb->fw.use_tz = false;
	ab_ahb->fw.iommu_domain = iommu_dom;
	of_node_put(node);

	return 0;

err_iommu_unmap:
	iommu_unmap(iommu_dom, ab_ahb->fw.msa_paddr, ab_ahb->fw.msa_size);

err_iommu_detach:
	iommu_detach_device(iommu_dom, ab_ahb->fw.dev);

err_iommu_free:
	iommu_domain_free(iommu_dom);

err_unregister:
	platform_device_unregister(pdev);
	of_node_put(node);

	return ret;
}

static int ath11k_ahb_fw_resource_deinit(struct ath11k_base *ab)
{
	struct ath11k_ahb *ab_ahb = ath11k_ahb_priv(ab);
	struct iommu_domain *iommu;
	size_t unmapped_size;

	if (ab_ahb->fw.use_tz)
		return 0;

	iommu = ab_ahb->fw.iommu_domain;

	unmapped_size = iommu_unmap(iommu, ab_ahb->fw.msa_paddr, ab_ahb->fw.msa_size);
	if (unmapped_size != ab_ahb->fw.msa_size)
		ath11k_err(ab, "failed to unmap firmware: %zu\n",
			   unmapped_size);

	unmapped_size = iommu_unmap(iommu, ab_ahb->fw.ce_paddr, ab_ahb->fw.ce_size);
	if (unmapped_size != ab_ahb->fw.ce_size)
		ath11k_err(ab, "failed to unmap firmware CE memory: %zu\n",
			   unmapped_size);

	iommu_detach_device(iommu, ab_ahb->fw.dev);
	iommu_domain_free(iommu);

	platform_device_unregister(to_platform_device(ab_ahb->fw.dev));

	return 0;
}

static int ath11k_ahb_probe(struct platform_device *pdev)
{
	struct ath11k_base *ab;
	const struct of_device_id *of_id;
	const struct ath11k_hif_ops *hif_ops;
	const struct ath11k_pci_ops *pci_ops;
	enum ath11k_hw_rev hw_rev;
	int ret;

	of_id = of_match_device(ath11k_ahb_of_match, &pdev->dev);
	if (!of_id) {
		dev_err(&pdev->dev, "failed to find matching device tree id\n");
		return -EINVAL;
	}

	hw_rev = (enum ath11k_hw_rev)of_id->data;

	switch (hw_rev) {
	case ATH11K_HW_IPQ8074:
	case ATH11K_HW_IPQ6018_HW10:
		hif_ops = &ath11k_ahb_hif_ops_ipq8074;
		pci_ops = NULL;
		break;
	case ATH11K_HW_WCN6750_HW10:
		hif_ops = &ath11k_ahb_hif_ops_wcn6750;
		pci_ops = &ath11k_ahb_pci_ops_wcn6750;
		break;
	default:
		dev_err(&pdev->dev, "unsupported device type %d\n", hw_rev);
		return -EOPNOTSUPP;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "failed to set 32-bit consistent dma\n");
		return ret;
	}

	ab = ath11k_core_alloc(&pdev->dev, sizeof(struct ath11k_ahb),
			       ATH11K_BUS_AHB);
	if (!ab) {
		dev_err(&pdev->dev, "failed to allocate ath11k base\n");
		return -ENOMEM;
	}

	ab->hif.ops = hif_ops;
	ab->pci.ops = pci_ops;
	ab->pdev = pdev;
	ab->hw_rev = hw_rev;
	platform_set_drvdata(pdev, ab);

	ret = ath11k_ahb_setup_resources(ab);
	if (ret)
		goto err_core_free;

	ret = ath11k_core_pre_init(ab);
	if (ret)
		goto err_core_free;

	ret = ath11k_ahb_fw_resources_init(ab);
	if (ret)
		goto err_core_free;

	ret = ath11k_hal_srng_init(ab);
	if (ret)
		goto err_fw_deinit;

	ret = ath11k_ce_alloc_pipes(ab);
	if (ret) {
		ath11k_err(ab, "failed to allocate ce pipes: %d\n", ret);
		goto err_hal_srng_deinit;
	}

	ath11k_ahb_init_qmi_ce_config(ab);

	ret = ath11k_core_get_rproc(ab);
	if (ret) {
		ath11k_err(ab, "failed to get rproc: %d\n", ret);
		goto err_ce_free;
	}

	ret = ath11k_core_init(ab);
	if (ret) {
		ath11k_err(ab, "failed to init core: %d\n", ret);
		goto err_ce_free;
	}

	ret = ath11k_ahb_config_irq(ab);
	if (ret) {
		ath11k_err(ab, "failed to configure irq: %d\n", ret);
		goto err_ce_free;
	}

	ath11k_ahb_fwreset_from_cold_boot(ab);

	return 0;

err_ce_free:
	ath11k_ce_free_pipes(ab);

err_hal_srng_deinit:
	ath11k_hal_srng_deinit(ab);

err_fw_deinit:
	ath11k_ahb_fw_resource_deinit(ab);

err_core_free:
	ath11k_core_free(ab);
	platform_set_drvdata(pdev, NULL);

	return ret;
}

static int ath11k_ahb_remove(struct platform_device *pdev)
{
	struct ath11k_base *ab = platform_get_drvdata(pdev);
	unsigned long left;

	if (test_bit(ATH11K_FLAG_QMI_FAIL, &ab->dev_flags)) {
		ath11k_ahb_power_down(ab);
		ath11k_debugfs_soc_destroy(ab);
		ath11k_qmi_deinit_service(ab);
		goto qmi_fail;
	}

	reinit_completion(&ab->driver_recovery);

	if (test_bit(ATH11K_FLAG_RECOVERY, &ab->dev_flags)) {
		left = wait_for_completion_timeout(&ab->driver_recovery,
						   ATH11K_AHB_RECOVERY_TIMEOUT);
		if (!left)
			ath11k_warn(ab, "failed to receive recovery response completion\n");
	}

	set_bit(ATH11K_FLAG_UNREGISTERING, &ab->dev_flags);
	cancel_work_sync(&ab->restart_work);

	ath11k_core_deinit(ab);
qmi_fail:
	ath11k_ahb_free_irq(ab);
	ath11k_hal_srng_deinit(ab);
	ath11k_ahb_fw_resource_deinit(ab);
	ath11k_ce_free_pipes(ab);
	ath11k_core_free(ab);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver ath11k_ahb_driver = {
	.driver         = {
		.name   = "ath11k",
		.of_match_table = ath11k_ahb_of_match,
	},
	.probe  = ath11k_ahb_probe,
	.remove = ath11k_ahb_remove,
};

static int ath11k_ahb_init(void)
{
	return platform_driver_register(&ath11k_ahb_driver);
}
module_init(ath11k_ahb_init);

static void ath11k_ahb_exit(void)
{
	platform_driver_unregister(&ath11k_ahb_driver);
}
module_exit(ath11k_ahb_exit);

MODULE_DESCRIPTION("Driver support for Qualcomm Technologies 802.11ax WLAN AHB devices");
MODULE_LICENSE("Dual BSD/GPL");
