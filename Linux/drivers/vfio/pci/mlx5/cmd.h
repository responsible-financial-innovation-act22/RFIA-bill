/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef MLX5_VFIO_CMD_H
#define MLX5_VFIO_CMD_H

#include <linux/kernel.h>
#include <linux/vfio_pci_core.h>
#include <linux/mlx5/driver.h>

struct mlx5vf_async_data {
	struct mlx5_async_work cb_work;
	struct work_struct work;
	int status;
	u32 pdn;
	u32 mkey;
	void *out;
};

struct mlx5_vf_migration_file {
	struct file *filp;
	struct mutex lock;
	u8 disabled:1;
	u8 is_err:1;

	struct sg_append_table table;
	size_t total_length;
	size_t allocated_length;

	/* Optimize mlx5vf_get_migration_page() for sequential access */
	struct scatterlist *last_offset_sg;
	unsigned int sg_last_entry;
	unsigned long last_offset;
	struct mlx5vf_pci_core_device *mvdev;
	wait_queue_head_t poll_wait;
	struct mlx5_async_ctx async_ctx;
	struct mlx5vf_async_data async_data;
};

struct mlx5vf_pci_core_device {
	struct vfio_pci_core_device core_device;
	int vf_id;
	u16 vhca_id;
	u8 migrate_cap:1;
	u8 deferred_reset:1;
	u8 mdev_detach:1;
	/* protect migration state */
	struct mutex state_mutex;
	enum vfio_device_mig_state mig_state;
	/* protect the reset_done flow */
	spinlock_t reset_lock;
	struct mlx5_vf_migration_file *resuming_migf;
	struct mlx5_vf_migration_file *saving_migf;
	struct workqueue_struct *cb_wq;
	struct notifier_block nb;
	struct mlx5_core_dev *mdev;
};

int mlx5vf_cmd_suspend_vhca(struct mlx5vf_pci_core_device *mvdev, u16 op_mod);
int mlx5vf_cmd_resume_vhca(struct mlx5vf_pci_core_device *mvdev, u16 op_mod);
int mlx5vf_cmd_query_vhca_migration_state(struct mlx5vf_pci_core_device *mvdev,
					  size_t *state_size);
void mlx5vf_cmd_set_migratable(struct mlx5vf_pci_core_device *mvdev);
void mlx5vf_cmd_remove_migratable(struct mlx5vf_pci_core_device *mvdev);
int mlx5vf_cmd_save_vhca_state(struct mlx5vf_pci_core_device *mvdev,
			       struct mlx5_vf_migration_file *migf);
int mlx5vf_cmd_load_vhca_state(struct mlx5vf_pci_core_device *mvdev,
			       struct mlx5_vf_migration_file *migf);
void mlx5vf_state_mutex_unlock(struct mlx5vf_pci_core_device *mvdev);
void mlx5vf_disable_fds(struct mlx5vf_pci_core_device *mvdev);
void mlx5vf_mig_file_cleanup_cb(struct work_struct *_work);
#endif /* MLX5_VFIO_CMD_H */
