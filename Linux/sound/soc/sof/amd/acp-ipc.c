// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license. When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Advanced Micro Devices, Inc.
//
// Authors: Balakishore Pati <Balakishore.pati@amd.com>
//	    Ajit Kumar Pandey <AjitKumar.Pandey@amd.com>

/* ACP-specific SOF IPC code */

#include <linux/module.h>
#include "../ops.h"
#include "acp.h"
#include "acp-dsp-offset.h"

void acp_mailbox_write(struct snd_sof_dev *sdev, u32 offset, void *message, size_t bytes)
{
	memcpy_to_scratch(sdev, offset, message, bytes);
}
EXPORT_SYMBOL_NS(acp_mailbox_write, SND_SOC_SOF_AMD_COMMON);

void acp_mailbox_read(struct snd_sof_dev *sdev, u32 offset, void *message, size_t bytes)
{
	memcpy_from_scratch(sdev, offset, message, bytes);
}
EXPORT_SYMBOL_NS(acp_mailbox_read, SND_SOC_SOF_AMD_COMMON);

static void acpbus_trigger_host_to_dsp_swintr(struct acp_dev_data *adata)
{
	struct snd_sof_dev *sdev = adata->dev;
	u32 swintr_trigger;

	swintr_trigger = snd_sof_dsp_read(sdev, ACP_DSP_BAR, ACP_SW_INTR_TRIG);
	swintr_trigger |= 0x01;
	snd_sof_dsp_write(sdev, ACP_DSP_BAR, ACP_SW_INTR_TRIG, swintr_trigger);
}

static void acp_ipc_host_msg_set(struct snd_sof_dev *sdev)
{
	unsigned int host_msg = offsetof(struct scratch_ipc_conf, sof_host_msg_write);

	snd_sof_dsp_write(sdev, ACP_DSP_BAR, ACP_SCRATCH_REG_0 + host_msg, 1);
}

static void acp_dsp_ipc_host_done(struct snd_sof_dev *sdev)
{
	unsigned int dsp_msg = offsetof(struct scratch_ipc_conf, sof_dsp_msg_write);

	snd_sof_dsp_write(sdev, ACP_DSP_BAR, ACP_SCRATCH_REG_0 + dsp_msg, 0);
}

static void acp_dsp_ipc_dsp_done(struct snd_sof_dev *sdev)
{
	unsigned int dsp_ack = offsetof(struct scratch_ipc_conf, sof_dsp_ack_write);

	snd_sof_dsp_write(sdev, ACP_DSP_BAR, ACP_SCRATCH_REG_0 + dsp_ack, 0);
}

int acp_sof_ipc_send_msg(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	struct acp_dev_data *adata = sdev->pdata->hw_pdata;
	unsigned int offset = offsetof(struct scratch_ipc_conf, sof_in_box);
	unsigned int count = ACP_HW_SEM_RETRY_COUNT;

	while (snd_sof_dsp_read(sdev, ACP_DSP_BAR, ACP_AXI2DAGB_SEM_0)) {
		/* Wait until acquired HW Semaphore Lock or timeout*/
		count--;
		if (!count) {
			dev_err(sdev->dev, "%s: Failed to acquire HW lock\n", __func__);
			return -EINVAL;
		}
	}

	acp_mailbox_write(sdev, offset, msg->msg_data, msg->msg_size);
	acp_ipc_host_msg_set(sdev);

	/* Trigger host to dsp interrupt for the msg */
	acpbus_trigger_host_to_dsp_swintr(adata);

	/* Unlock or Release HW Semaphore */
	snd_sof_dsp_write(sdev, ACP_DSP_BAR, ACP_AXI2DAGB_SEM_0, 0x0);

	return 0;
}
EXPORT_SYMBOL_NS(acp_sof_ipc_send_msg, SND_SOC_SOF_AMD_COMMON);

static void acp_dsp_ipc_get_reply(struct snd_sof_dev *sdev)
{
	struct snd_sof_ipc_msg *msg = sdev->msg;
	struct sof_ipc_reply reply;
	struct sof_ipc_cmd_hdr *hdr;
	unsigned int offset = offsetof(struct scratch_ipc_conf, sof_in_box);
	int ret = 0;

       /*
	* Sometimes, there is unexpected reply ipc arriving. The reply
	* ipc belongs to none of the ipcs sent from driver.
	* In this case, the driver must ignore the ipc.
	*/
	if (!msg) {
		dev_warn(sdev->dev, "unexpected ipc interrupt raised!\n");
		return;
	}
	hdr = msg->msg_data;
	if (hdr->cmd == (SOF_IPC_GLB_PM_MSG | SOF_IPC_PM_CTX_SAVE) ||
	    hdr->cmd == (SOF_IPC_GLB_PM_MSG | SOF_IPC_PM_GATE)) {
		/*
		 * memory windows are powered off before sending IPC reply,
		 * so we can't read the mailbox for CTX_SAVE and PM_GATE
		 * replies.
		 */
		reply.error = 0;
		reply.hdr.cmd = SOF_IPC_GLB_REPLY;
		reply.hdr.size = sizeof(reply);
		memcpy(msg->reply_data, &reply, sizeof(reply));
		goto out;
	}
	/* get IPC reply from DSP in the mailbox */
	acp_mailbox_read(sdev, offset, &reply, sizeof(reply));
	if (reply.error < 0) {
		memcpy(msg->reply_data, &reply, sizeof(reply));
		ret = reply.error;
	} else {
		/* reply correct size ? */
		if (reply.hdr.size != msg->reply_size &&
		    !(reply.hdr.cmd & SOF_IPC_GLB_PROBE)) {
			dev_err(sdev->dev, "reply expected %zu got %u bytes\n",
				msg->reply_size, reply.hdr.size);
			ret = -EINVAL;
		}
		/* read the message */
		if (msg->reply_size > 0)
			acp_mailbox_read(sdev, offset, msg->reply_data, msg->reply_size);
	}
out:
	msg->reply_error = ret;
}

irqreturn_t acp_sof_ipc_irq_thread(int irq, void *context)
{
	struct snd_sof_dev *sdev = context;
	unsigned int dsp_msg_write = offsetof(struct scratch_ipc_conf, sof_dsp_msg_write);
	unsigned int dsp_ack_write = offsetof(struct scratch_ipc_conf, sof_dsp_ack_write);
	bool ipc_irq = false;
	int dsp_msg, dsp_ack;

	dsp_msg = snd_sof_dsp_read(sdev, ACP_DSP_BAR, ACP_SCRATCH_REG_0 + dsp_msg_write);
	if (dsp_msg) {
		snd_sof_ipc_msgs_rx(sdev);
		acp_dsp_ipc_host_done(sdev);
		ipc_irq = true;
	}

	dsp_ack = snd_sof_dsp_read(sdev, ACP_DSP_BAR, ACP_SCRATCH_REG_0 + dsp_ack_write);
	if (dsp_ack) {
		spin_lock_irq(&sdev->ipc_lock);
		/* handle immediate reply from DSP core */
		acp_dsp_ipc_get_reply(sdev);
		snd_sof_ipc_reply(sdev, 0);
		/* set the done bit */
		acp_dsp_ipc_dsp_done(sdev);
		spin_unlock_irq(&sdev->ipc_lock);
		ipc_irq = true;
	}

	if (!ipc_irq)
		dev_dbg_ratelimited(sdev->dev, "nothing to do in IPC IRQ thread\n");

	return IRQ_HANDLED;
}
EXPORT_SYMBOL_NS(acp_sof_ipc_irq_thread, SND_SOC_SOF_AMD_COMMON);

int acp_sof_ipc_msg_data(struct snd_sof_dev *sdev, struct snd_pcm_substream *substream,
			 void *p, size_t sz)
{
	unsigned int offset = offsetof(struct scratch_ipc_conf, sof_out_box);

	if (!substream || !sdev->stream_box.size)
		acp_mailbox_read(sdev, offset, p, sz);

	return 0;
}
EXPORT_SYMBOL_NS(acp_sof_ipc_msg_data, SND_SOC_SOF_AMD_COMMON);

int acp_sof_ipc_get_mailbox_offset(struct snd_sof_dev *sdev)
{
	return ACP_SCRATCH_MEMORY_ADDRESS;
}
EXPORT_SYMBOL_NS(acp_sof_ipc_get_mailbox_offset, SND_SOC_SOF_AMD_COMMON);

MODULE_DESCRIPTION("AMD ACP sof-ipc driver");
