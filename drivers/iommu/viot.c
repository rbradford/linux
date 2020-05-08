// SPDX-License-Identifier: GPL-2.0
/*
 * Mostly copied from drivers/acpi/arm64/iort.c
 */
#define pr_fmt(fmt) "ACPI: VIOT: " fmt

#include <linux/acpi_viot.h>
#include <linux/fwnode.h>
#include <linux/list.h>
#include <linux/virt_iommu.h>

/*
 * Keep track of IOMMU nodes already visited, while parsing.
 */
struct viot_iommu {
	struct virt_iommu_spec spec;
	struct list_head list;
	unsigned int offset;
};

static struct acpi_table_viot *viot;
static LIST_HEAD(iommus);

static int viot_check_bounds(const struct acpi_viot_node *node)
{
	struct acpi_viot_node *start, *end;

	start = ACPI_ADD_PTR(struct acpi_viot_node, viot, viot->node_offset);
	end = ACPI_ADD_PTR(struct acpi_viot_node, viot, viot->header.length);

	if (node < start || node >= end) {
		pr_err("Node pointer overflows, bad table\n");
		return -EOVERFLOW;
	}
	if (node->length < sizeof(*node)) {
		pr_err("Empty node, bad table\n");
		return -EINVAL;
	}
	return 0;	
}

static struct virt_iommu_spec *viot_get_iommu(unsigned int offset)
{
	struct viot_iommu *viommu;
	struct acpi_viot_node *node = ACPI_ADD_PTR(struct acpi_viot_node, viot,
						   offset);
	union {
		struct acpi_viot_virtio_iommu_pci pci;
		struct acpi_viot_virtio_iommu_mmio mmio;
	} *cfg = (void *)node;

	list_for_each_entry(viommu, &iommus, list)
		if (viommu->offset == offset)
			return &viommu->spec;

	if (viot_check_bounds(node))
		return NULL;

	viommu = kzalloc(sizeof(*viommu), GFP_KERNEL);
	if (!viommu)
		return NULL;

	viommu->offset = offset;
	switch (node->type) {
	case ACPI_VIOT_NODE_VIRTIO_IOMMU_PCI:
		if (node->length < sizeof(cfg->pci))
			goto err_free;

		viommu->spec.devid.type = VIRT_IOMMU_DEV_TYPE_PCI;
		viommu->spec.devid.segment = cfg->pci.segment;
		viommu->spec.devid.bdf_start = cfg->pci.bdf;
		viommu->spec.devid.bdf_end = cfg->pci.bdf;
		break;
	case ACPI_VIOT_NODE_VIRTIO_IOMMU_MMIO:
		if (node->length < sizeof(cfg->mmio))
			goto err_free;

		viommu->spec.devid.type = VIRT_IOMMU_DEV_TYPE_MMIO;
		viommu->spec.devid.base = cfg->mmio.base_address;
		break;
	default:
		kfree(viommu);
		return NULL;
	}

	list_add(&viommu->list, &iommus);
	virt_iommu_add_iommu_spec(&viommu->spec);
	return &viommu->spec;

err_free:
	kfree(viommu);
	return NULL;
}

static int __init viot_parse_node(const struct acpi_viot_node *node)
{
	int ret = -EINVAL;
	struct virt_iommu_endpoint_spec *spec;
	union {
		struct acpi_viot_mmio mmio;
		struct acpi_viot_pci_range pci;
	} *cfg = (void *)node;

	if (viot_check_bounds(node))
		return -EINVAL;

	if (node->reserved)
		pr_warn("unexpected reserved data in node\n");

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return -ENOMEM;

	switch (node->type) {
	case ACPI_VIOT_NODE_PCI_RANGE:
		if (node->length < sizeof(cfg->pci))
			goto err_free;

		spec->devid.type = VIRT_IOMMU_DEV_TYPE_PCI;
		spec->devid.segment = cfg->pci.segment;
		spec->devid.bdf_start = cfg->pci.bdf_start;
		spec->devid.bdf_end = cfg->pci.bdf_end;
		spec->endpoint_id = cfg->pci.endpoint_start;
		spec->viommu = viot_get_iommu(cfg->pci.output_node);
		break;
	case ACPI_VIOT_NODE_MMIO:
		if (node->length < sizeof(cfg->mmio))
			goto err_free;

		spec->devid.type = VIRT_IOMMU_DEV_TYPE_MMIO;
		spec->devid.base = cfg->mmio.base_address;
		spec->endpoint_id = cfg->mmio.endpoint;
		spec->viommu = viot_get_iommu(cfg->mmio.output_node);
		break;
	default:
		ret = 0;
		goto err_free;
	}

	if (!spec->viommu) {
		ret = -ENODEV;
		goto err_free;
	}

	virt_iommu_add_endpoint_spec(spec);
	return 0;

err_free:
	kfree(spec);
	return ret;
}

static void __init viot_parse_nodes(void)
{
	int i;
	struct acpi_viot_node *node;

	if (viot->node_offset < sizeof(*viot)) {
		pr_err("Invalid node offset, bad table\n");
		return;
	}

	node = ACPI_ADD_PTR(struct acpi_viot_node, viot, viot->node_offset);

	for (i = 0; i < viot->node_count; i++) {
		if (viot_parse_node(node))
			return;

		node = ACPI_ADD_PTR(struct acpi_viot_node, node, node->length);
	}
}

void __init acpi_viot_init(void)
{
	acpi_status status;
	struct acpi_table_header *hdr;

	status = acpi_get_table(ACPI_SIG_VIOT, 0, &hdr);
	if (ACPI_FAILURE(status)) {
		if (status != AE_NOT_FOUND) {
			const char *msg = acpi_format_exception(status);

			pr_err("Failed to get table, %s\n", msg);
		}
		return;
	}

	viot = (void *)hdr;
	viot_parse_nodes();
}
