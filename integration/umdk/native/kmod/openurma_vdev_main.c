// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/types.h>
#include <net/net_namespace.h>
#include <ub/urma/ubcore_api.h>
#include <ub/urma/ubcore_types.h>

#include "../include/openurma_vdev_abi.h"
#include "openurma_vdev.h"

#define OPENURMA_VDEV_NAME "openurma_vdev0"
#define OPENURMA_VDEV_DRIVER "openurma_vdev"

static struct openurma_vdev g_vdev;

static int openurma_vdev_install_local_eid(struct openurma_vdev *vdev)
{
	struct ubcore_eid_entry *entry, *entry2;

	if (!vdev->ubdev.eid_table.eid_entries)
		return -EINVAL;
	spin_lock(&vdev->ubdev.eid_table.lock);
	entry = &vdev->ubdev.eid_table.eid_entries[0];
	entry->eid.raw[0] = 0xfd;
	entry->eid.raw[15] = 1;
	entry->eid_index = 0;
	entry->net = &init_net;
	entry->valid = true;
	entry2 = &vdev->ubdev.eid_table.eid_entries[1];
	entry2->eid.raw[0] = 0xfd;
	entry2->eid.raw[15] = 2;
	entry2->eid_index = 1;
	entry2->net = &init_net;
	entry2->valid = true;
	spin_unlock(&vdev->ubdev.eid_table.lock);
	return 0;
}

static inline struct openurma_vdev *to_openurma_vdev(struct ubcore_device *dev)
{
	return container_of(dev, struct openurma_vdev, ubdev);
}

static int openurma_vdev_query_device_attr(struct ubcore_device *dev,
					   struct ubcore_device_attr *attr)
{
	struct openurma_vdev *vdev = to_openurma_vdev(dev);

	/*
	 * ubcore passes &dev->attr here, including from concurrent sysfs reads.
	 * That object is also read locklessly by the create-object validation
	 * paths.  Clearing the whole shared object first therefore exposes a
	 * transient zero capability set and can make a valid create fail.  The
	 * device is zero-initialized before registration and these capabilities
	 * are immutable, so publish the individual nonzero fields directly.
	 */
	attr->dev_cap.max_eid_cnt = 2;
	attr->dev_cap.page_size_cap = PAGE_SIZE;
	attr->dev_cap.max_jfc = 256;
	attr->dev_cap.max_jfs = 256;
	attr->dev_cap.max_jfr = 256;
	attr->dev_cap.max_jetty = 256;
	attr->dev_cap.max_jfc_depth = 256;
	attr->dev_cap.max_jfr_depth = 256;
	attr->dev_cap.max_jfs_depth = 256;
	attr->dev_cap.max_jfr_sge = 1;
	attr->dev_cap.max_jfs_sge = 1;
	attr->dev_cap.max_jfs_rsge = 1;
	attr->dev_cap.max_msg_size = 10U * 1024U * 1024U;
	attr->dev_cap.max_read_size = 10U * 1024U * 1024U;
	attr->dev_cap.max_write_size = 10U * 1024U * 1024U;
	attr->dev_cap.max_cas_size = sizeof(u64);
	attr->dev_cap.max_fetch_and_add_size = sizeof(u64);
	attr->dev_cap.atomic_feat.bs.cas = 1;
	attr->dev_cap.atomic_feat.bs.fetch_and_add = 1;
	/* Bound all official mode-specific VTPN tables to the advertised Q4
	 * control-plane capacity.  The core owns their allocation and teardown.
	 */
	attr->dev_cap.max_vtp_cnt_per_ue = 256;
	attr->dev_cap.trans_mode = UBCORE_TP_RC;
	attr->reserved_jetty_id_min = READ_ONCE(vdev->reserved_jetty_id_min);
	attr->reserved_jetty_id_max = READ_ONCE(vdev->reserved_jetty_id_max);
	attr->port_attr[0].max_mtu = UBCORE_MTU_4096;
	attr->port_cnt = 1;
	attr->virtualization = true;
	return 0;
}

static int openurma_vdev_query_device_status(struct ubcore_device *dev,
					     struct ubcore_device_status *status)
{
	(void)dev;
	memset(status, 0, sizeof(*status));
	status->port_status[0].state = UBCORE_PORT_ACTIVE;
	status->port_status[0].active_mtu = UBCORE_MTU_4096;
	status->port_status[0].active_width = UBCORE_LINK_X1;
	status->port_status[0].active_speed = UBCORE_SP_10M;
	return 0;
}

static int openurma_vdev_config_device(struct ubcore_device *dev,
				       struct ubcore_device_cfg *cfg)
{
	struct openurma_vdev *vdev = to_openurma_vdev(dev);
	union ubcore_device_cfg_mask allowed = { .value = 0 };

	allowed.bs.rc_cnt = 1;
	allowed.bs.rc_depth = 1;
	allowed.bs.reserved_jetty_id_min = 1;
	allowed.bs.reserved_jetty_id_max = 1;
	if (cfg->mask.value & ~allowed.value)
		return -EOPNOTSUPP;
	if ((cfg->mask.bs.rc_cnt && cfg->rc_cfg.rc_cnt != 0) ||
	    (cfg->mask.bs.rc_depth && cfg->rc_cfg.depth != 0))
		return -EOPNOTSUPP;
	if (cfg->mask.bs.reserved_jetty_id_min)
		vdev->reserved_jetty_id_min = cfg->reserved_jetty_id_min;
	if (cfg->mask.bs.reserved_jetty_id_max)
		vdev->reserved_jetty_id_max = cfg->reserved_jetty_id_max;
	return 0;
}

static struct ubcore_ops g_vdev_ops = {
	.owner = THIS_MODULE,
	.driver_name = OPENURMA_VDEV_DRIVER,
	.abi_version = 1,
	.query_device_attr = openurma_vdev_query_device_attr,
	.query_device_status = openurma_vdev_query_device_status,
	.config_device = openurma_vdev_config_device,
	.alloc_ucontext = openurma_vdev_alloc_ucontext,
	.free_ucontext = openurma_vdev_free_ucontext,
	.mmap = openurma_vdev_mmap,
	.alloc_token_id = openurma_vdev_alloc_token_id,
	.free_token_id = openurma_vdev_free_token_id,
	.register_seg = openurma_vdev_register_seg,
	.unregister_seg = openurma_vdev_unregister_seg,
	.import_seg = openurma_vdev_import_seg,
	.unimport_seg = openurma_vdev_unimport_seg,
	.create_jfc = openurma_vdev_create_jfc,
	.destroy_jfc = openurma_vdev_destroy_jfc,
	.create_jfs = openurma_vdev_create_jfs,
	.destroy_jfs = openurma_vdev_destroy_jfs,
	.create_jfr = openurma_vdev_create_jfr,
	.destroy_jfr = openurma_vdev_destroy_jfr,
	.create_jetty = openurma_vdev_create_jetty,
	.destroy_jetty = openurma_vdev_destroy_jetty,
	.import_jetty = openurma_vdev_import_jetty,
	.unimport_jetty = openurma_vdev_unimport_jetty,
	.bind_jetty = openurma_vdev_bind_jetty,
	.unbind_jetty = openurma_vdev_unbind_jetty,
	.alloc_vtpn = openurma_vdev_alloc_vtpn,
	.free_vtpn = openurma_vdev_free_vtpn,
	.user_ctl = openurma_vdev_user_ctl,
};

static int __init openurma_vdev_init(void)
{
	struct openurma_vdev *vdev = &g_vdev;
	int ret;

	memset(vdev, 0, sizeof(*vdev));
	vdev->abi_version = OPENURMA_VDEV_ABI_VERSION;
	vdev->reserved_jetty_id_min = U32_MAX;
	vdev->reserved_jetty_id_max = U32_MAX;
	openurma_vdev_control_init(vdev);
	ret = openurma_vdev_model_init(vdev);
	if (ret) {
		pr_err("openurma_vdev: model initialization failed: %d\n", ret);
		goto err_control;
	}
	vdev->pdev = platform_device_register_simple("openurma-vdev", -1, NULL, 0);
	if (IS_ERR(vdev->pdev)) {
		ret = PTR_ERR(vdev->pdev);
		vdev->pdev = NULL;
		goto err_model;
	}

	ret = dma_coerce_mask_and_coherent(&vdev->pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto err_platform;

	strscpy(vdev->ubdev.dev_name, OPENURMA_VDEV_NAME,
		UBCORE_MAX_DEV_NAME);
	vdev->ubdev.dma_dev = &vdev->pdev->dev;
	vdev->ubdev.dev.parent = &vdev->pdev->dev;
	vdev->ubdev.ops = &g_vdev_ops;
	vdev->ubdev.transport_type = UBCORE_TRANSPORT_UB;

	ret = openurma_vdev_debugfs_init(vdev);
	if (ret)
		goto err_platform;
	ret = ubcore_register_device(&vdev->ubdev);
	if (ret)
		goto err_debugfs;
	/* Q4 needs one local EID for public control-lifecycle ioctls.  It is
	 * deliberately not dispatched to UB MAD/UVS: no hardware control plane
	 * exists in this virtual device phase.
	 */
	ret = openurma_vdev_install_local_eid(vdev);
	if (ret)
		goto err_unregister;
	vdev->devices = 1;
	pr_info("openurma_vdev: registered %s mode=%s qd=%u node=%d cpu=%d\n",
		OPENURMA_VDEV_NAME, vdev->model_mode_name,
		vdev->model_queue_depth, vdev->model_numa_node,
		vdev->model_worker_cpu);
	return 0;

err_unregister:
	ubcore_unregister_device(&vdev->ubdev);
err_debugfs:
	openurma_vdev_debugfs_fini(vdev);
err_platform:
	platform_device_unregister(vdev->pdev);
	vdev->pdev = NULL;
err_model:
	openurma_vdev_model_fini(vdev);
err_control:
	openurma_vdev_control_fini(vdev);
	return ret;
}

static void __exit openurma_vdev_exit(void)
{
	struct openurma_vdev *vdev = &g_vdev;

	vdev->devices = 0;
	ubcore_unregister_device(&vdev->ubdev);
	openurma_vdev_debugfs_fini(vdev);
	platform_device_unregister(vdev->pdev);
	vdev->pdev = NULL;
	openurma_vdev_model_fini(vdev);
	openurma_vdev_control_fini(vdev);
	pr_info("openurma_vdev: unregistered %s\n", OPENURMA_VDEV_NAME);
}

module_init(openurma_vdev_init);
module_exit(openurma_vdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("UltraShuffle native-vdev project");
MODULE_DESCRIPTION("Virtual URMA device with functional and timed-NUMA models");
MODULE_VERSION("0.7-timed-numa");
