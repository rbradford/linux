// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/virtio_ids.h>
#include <linux/virt_iommu.h>
#include <linux/virtio_pci.h>
#include <uapi/linux/virtio_iommu.h>

struct viommu_cap_config {
	u8 bar;
	u32 length; /* structure size */
	u32 offset; /* structure offset within the bar */
};

struct viommu_topo_header {
	u8 type;
	u8 reserved;
	u16 length;
};

static struct virt_iommu_endpoint_spec *
viommu_parse_node(void __iomem *buf, size_t len)
{
	int ret = -EINVAL;
	union {
		struct viommu_topo_header hdr;
		struct virtio_iommu_topo_pci_range pci;
		struct virtio_iommu_topo_mmio mmio;
	} __iomem *cfg = buf;
	struct virt_iommu_endpoint_spec *spec;

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return ERR_PTR(-ENOMEM);

	switch (ioread8(&cfg->hdr.type)) {
	case VIRTIO_IOMMU_TOPO_PCI_RANGE:
		if (len < sizeof(cfg->pci))
			goto err_free;

		spec->devid.type = VIRT_IOMMU_DEV_TYPE_PCI;
		spec->devid.segment = ioread16(&cfg->pci.segment);
		spec->devid.bdf_start = ioread16(&cfg->pci.bdf_start);
		spec->devid.bdf_end = ioread16(&cfg->pci.bdf_end);
		spec->endpoint_id = ioread32(&cfg->pci.endpoint_start);
		break;
	case VIRTIO_IOMMU_TOPO_MMIO:
		if (len < sizeof(cfg->mmio))
			goto err_free;

		spec->devid.type = VIRT_IOMMU_DEV_TYPE_MMIO;
		spec->devid.base = ioread64(&cfg->mmio.address);
		spec->endpoint_id = ioread32(&cfg->mmio.endpoint);
		break;
	default:
		goto err_free;
	}
	return spec;

err_free:
	kfree(spec);
	return ERR_PTR(ret);
}

static int viommu_parse_topology(struct device *dev,
				 struct virtio_iommu_config __iomem *cfg,
				 size_t max_len)
{
	int ret;
	u16 len;
	size_t i;
	LIST_HEAD(endpoints);
	size_t offset, num_items;
	struct virt_iommu_endpoint_spec *ep, *next;
	struct virt_iommu_spec *viommu_spec;
	struct viommu_topo_header __iomem *cur;

	offset = ioread16(&cfg->topo_config.offset);
	num_items = ioread16(&cfg->topo_config.num_items);
	if (!offset || !num_items)
		return 0;

	viommu_spec = kzalloc(sizeof(*viommu_spec), GFP_KERNEL);
	if (!viommu_spec)
		return -ENOMEM;

	viommu_spec->dev = dev;

	for (i = 0; i < num_items; i++, offset += len) {
		if (offset + sizeof(*cur) > max_len) {
			ret = -EOVERFLOW;
			goto err_free;
		}

		cur = (void __iomem *)cfg + offset;
		len = ioread16(&cur->length);
		if (offset + len > max_len) {
			ret = -EOVERFLOW;
			goto err_free;
		}

		ep = viommu_parse_node((void __iomem *)cur, len);
		if (!ep) {
			continue;
		} else if (IS_ERR(ep)) {
			ret = PTR_ERR(ep);
			goto err_free;
		}

		ep->viommu = viommu_spec;
		list_add(&ep->list, &endpoints);
	}

	list_for_each_entry_safe(ep, next, &endpoints, list)
		/* Moves ep to the helpers list */
		virt_iommu_add_endpoint_spec(ep);
	virt_iommu_add_iommu_spec(viommu_spec);

	return 0;
err_free:
	list_for_each_entry_safe(ep, next, &endpoints, list)
		kfree(ep);
	kfree(viommu_spec);
	return ret;
}

#define VPCI_FIELD(field) offsetof(struct virtio_pci_cap, field)

static inline int viommu_pci_find_capability(struct pci_dev *dev, u8 cfg_type,
					     struct viommu_cap_config *cap)
{
	int pos;
	u8 bar;

	for (pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);
	     pos > 0;
	     pos = pci_find_next_capability(dev, pos, PCI_CAP_ID_VNDR)) {
		u8 type;

		pci_read_config_byte(dev, pos + VPCI_FIELD(cfg_type), &type);
		if (type != cfg_type)
			continue;

		pci_read_config_byte(dev, pos + VPCI_FIELD(bar), &bar);

		/* Ignore structures with reserved BAR values */
		if (type != VIRTIO_PCI_CAP_PCI_CFG && bar > 0x5)
			continue;

		cap->bar = bar;
		pci_read_config_dword(dev, pos + VPCI_FIELD(length),
				      &cap->length);
		pci_read_config_dword(dev, pos + VPCI_FIELD(offset),
				      &cap->offset);

		return pos;
	}
	return 0;
}

static void viommu_pci_parse_topology(struct pci_dev *dev)
{
	int ret;
	u32 features;
	void __iomem *regs;
	struct viommu_cap_config cap = {0};
	struct virtio_pci_common_cfg __iomem *common_cfg;

	/*
	 * The virtio infrastructure might not be loaded at this point. we need
	 * to access the BARs ourselves.
	 */
	ret = viommu_pci_find_capability(dev, VIRTIO_PCI_CAP_COMMON_CFG, &cap);
	if (!ret) {
		pci_warn(dev, "common capability not found\n");
		return;
	}

	if (pci_enable_device_mem(dev))
		return;

	regs = pci_iomap(dev, cap.bar, 0);
	if (!regs)
		return;

	common_cfg = regs + cap.offset;

	/* Find out if the device supports topology description */
	writel(0, &common_cfg->device_feature_select);
	features = ioread32(&common_cfg->device_feature);

	pci_iounmap(dev, regs);

	if (!(features & BIT(VIRTIO_IOMMU_F_TOPOLOGY))) {
		pci_dbg(dev, "device doesn't have topology description");
		return;
	}

	ret = viommu_pci_find_capability(dev, VIRTIO_PCI_CAP_DEVICE_CFG, &cap);
	if (!ret) {
		pci_warn(dev, "device config capability not found\n");
		return;
	}

	regs = pci_iomap(dev, cap.bar, 0);
	if (!regs)
		return;

	pci_info(dev, "parsing virtio-iommu topology\n");
	ret = viommu_parse_topology(&dev->dev, regs + cap.offset,
				    pci_resource_len(dev, 0) - cap.offset);
	if (ret)
		pci_warn(dev, "viommu_parse_topology() failed with %d\n", ret);
	pci_iounmap(dev, regs);
}

/*
 * Catch a PCI virtio-iommu implementation early to get the topology description
 * before we start probing other endpoints.
 */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_REDHAT_QUMRANET, 0x1040 + VIRTIO_ID_IOMMU,
			viommu_pci_parse_topology);
