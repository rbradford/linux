/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VIRT_IOMMU_H_
#define VIRT_IOMMU_H_

#ifdef CONFIG_VIRTIO_IOMMU_TOPOLOGY_HELPERS

/* Identify an endpoint or an IOMMU device */
struct virt_iommu_dev_spec {
	unsigned int			type;
#define VIRT_IOMMU_DEV_TYPE_PCI		1
#define VIRT_IOMMU_DEV_TYPE_MMIO	2
	union {
		/* PCI endpoint or range */
		struct {
			u16		segment;
			u16		bdf_start;
			u16		bdf_end;
		};
		/* MMIO region */
		u64			base;
	};
};

/* Specification of an IOMMU */
struct virt_iommu_spec {
	struct virt_iommu_dev_spec devid;
	struct device		*dev; /* transport device */
	struct fwnode_handle	*fwnode;
	struct iommu_ops	*ops;
	struct list_head	list;
};

/* Specification of an endpoint */
struct virt_iommu_endpoint_spec {
	struct virt_iommu_dev_spec devid;
	u32			endpoint_id;
	struct virt_iommu_spec	*viommu;
	struct list_head	list;
};

void virt_iommu_add_endpoint_spec(struct virt_iommu_endpoint_spec *spec);
void virt_iommu_add_iommu_spec(struct virt_iommu_spec *spec);

int virt_dma_configure(struct device *dev);
void virt_set_iommu_ops(struct device *dev, struct iommu_ops *ops);
#else /* !CONFIG_VIRTIO_IOMMU_TOPOLOGY_HELPERS */
static inline int virt_dma_configure(struct device *dev)
{
	/* Don't disturb the normal DMA configuration methods */
	return 0;
}

static inline void virt_set_iommu_ops(struct device *dev, struct iommu_ops *ops)
{ }
#endif /* !CONFIG_VIRTIO_IOMMU_TOPOLOGY_HELPERS */

#endif /* VIRT_IOMMU_H_ */
