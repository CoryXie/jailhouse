/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2013
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/mmio.h>
#include <jailhouse/paging.h>
#include <jailhouse/printk.h>
#include <asm/vtd.h>

/* TODO: Support multiple segments */
static struct vtd_entry __attribute__((aligned(PAGE_SIZE)))
	root_entry_table[256];
static void *dmar_reg_base;
static unsigned int dmar_units;
static unsigned int dmar_pt_levels;
static unsigned int dmar_num_did = ~0U;

static void *vtd_iotlb_reg_base(void *reg_base)
{
	return reg_base + ((mmio_read64(reg_base + VTD_ECAP_REG) &
			    VTD_ECAP_IRO_MASK) >> VTD_ECAP_IRO_SHIFT) * 16;
}

static void vtd_flush_dmar_caches(void *reg_base, u64 ctx_scope,
				  u64 iotlb_scope)
{
	void *iotlb_reg_base;

	mmio_write64(reg_base + VTD_CCMD_REG, ctx_scope | VTD_CCMD_ICC);
	while (mmio_read64(reg_base + VTD_CCMD_REG) & VTD_CCMD_ICC)
		cpu_relax();

	iotlb_reg_base = vtd_iotlb_reg_base(reg_base);
	mmio_write64(iotlb_reg_base + VTD_IOTLB_REG,
		     iotlb_scope | VTD_IOTLB_DW | VTD_IOTLB_DR |
		     VTD_IOTLB_IVT);
	while (mmio_read64(iotlb_reg_base + VTD_IOTLB_REG) & VTD_IOTLB_IVT)
		cpu_relax();
}

static void vtd_flush_domain_caches(unsigned int did)
{
	u64 iotlb_scope = VTD_IOTLB_IIRG_DOMAIN |
		((unsigned long)did << VTD_IOTLB_DID_SHIFT);
	void *reg_base = dmar_reg_base;
	unsigned int n;

	for (n = 0; n < dmar_units; n++, reg_base += PAGE_SIZE)
		vtd_flush_dmar_caches(reg_base, VTD_CCMD_CIRG_DOMAIN | did,
				      iotlb_scope);
}

int vtd_init(void)
{
	const struct acpi_dmar_table *dmar;
	const struct acpi_dmar_drhd *drhd;
	unsigned int pt_levels, num_did;
	void *reg_base = NULL;
	unsigned long offset;
	unsigned long caps;
	int err;

	dmar = (struct acpi_dmar_table *)acpi_find_table("DMAR", NULL);
	if (!dmar)
//		return -ENODEV;
		{ printk("WARNING: No VT-d support found!\n"); return 0; }

	if (sizeof(struct acpi_dmar_table) +
	    sizeof(struct acpi_dmar_drhd) > dmar->header.length)
		return -EIO;

	drhd = (struct acpi_dmar_drhd *)dmar->remap_structs;
	if (drhd->header.type != ACPI_DMAR_DRHD)
		return -EIO;

	offset = (void *)dmar->remap_structs - (void *)dmar;
	do {
		if (drhd->header.length < sizeof(struct acpi_dmar_drhd) ||
		    offset + drhd->header.length > dmar->header.length)
			return -EIO;

		/* TODO: support multiple segments */
		if (drhd->segment != 0)
			return -EIO;

		printk("Found DMAR @%p\n", drhd->register_base_addr);

		reg_base = page_alloc(&remap_pool, 1);
		if (!reg_base)
			return -ENOMEM;

		if (dmar_units == 0)
			dmar_reg_base = reg_base;
		else if (reg_base != dmar_reg_base + dmar_units * PAGE_SIZE)
			return -ENOMEM;

		err = page_map_create(hv_page_table, drhd->register_base_addr,
				      PAGE_SIZE, (unsigned long)reg_base,
				      PAGE_DEFAULT_FLAGS | PAGE_FLAG_UNCACHED,
				      PAGE_DEFAULT_FLAGS, PAGE_DIR_LEVELS,
				      PAGE_MAP_NON_COHERENT);
		if (err)
			return err;

		caps = mmio_read64(reg_base + VTD_CAP_REG);
		if (caps & VTD_CAP_SAGAW39)
			pt_levels = 3;
		else if (caps & VTD_CAP_SAGAW48)
			pt_levels = 4;
		else
			return -EIO;

		if (dmar_pt_levels > 0 && dmar_pt_levels != pt_levels)
			return -EIO;
		dmar_pt_levels = pt_levels;

		if (caps & VTD_CAP_CM)
			return -EIO;

		/* We only support IOTLB registers withing the first page. */
		if (vtd_iotlb_reg_base(reg_base) >= reg_base + PAGE_SIZE)
			return -EIO;

		if (mmio_read32(reg_base + VTD_GSTS_REG) & VTD_GSTS_TES)
			return -EBUSY;

		num_did = 1 << (4 + (caps & VTD_CAP_NUM_DID_MASK) * 2);
		if (num_did < dmar_num_did)
			dmar_num_did = num_did;

		dmar_units++;

		offset += drhd->header.length;
		drhd = (struct acpi_dmar_drhd *)
			(((void *)drhd) + drhd->header.length);
	} while (offset < dmar->header.length &&
		 drhd->header.type == ACPI_DMAR_DRHD);

	return 0;
}

static bool vtd_add_device_to_cell(struct cell *cell,
			           const struct jailhouse_pci_device *device)
{
	u64 root_entry_lo = root_entry_table[device->bus].lo_word;
	struct vtd_entry *context_entry_table, *context_entry;

	printk("Adding PCI device %02x:%02x.%x to cell \"%s\"\n",
	       device->bus, device->devfn >> 3, device->devfn & 7,
	       cell->config->name);

	if (root_entry_lo & VTD_ROOT_PRESENT) {
		context_entry_table =
			page_map_phys2hvirt(root_entry_lo & PAGE_MASK);
	} else {
		context_entry_table = page_alloc(&mem_pool, 1);
		if (!context_entry_table)
			return false;
		root_entry_table[device->bus].lo_word = VTD_ROOT_PRESENT |
			page_map_hvirt2phys(context_entry_table);
		flush_cache(&root_entry_table[device->bus].lo_word,
			    sizeof(u64));
	}

	context_entry = &context_entry_table[device->devfn];
	context_entry->lo_word = VTD_CTX_PRESENT |
		VTD_CTX_FPD | VTD_CTX_TTYPE_MLP_UNTRANS |
		page_map_hvirt2phys(cell->vtd.page_table);
	context_entry->hi_word =
		(dmar_pt_levels == 3 ? VTD_CTX_AGAW_39 : VTD_CTX_AGAW_48) |
		(cell->id << VTD_CTX_DID_SHIFT);
	flush_cache(context_entry, sizeof(*context_entry));

	return true;
}

int vtd_cell_init(struct cell *cell)
{
	struct jailhouse_cell_desc *config = cell->config;
	const struct jailhouse_memory *mem =
		jailhouse_cell_mem_regions(config);
	const struct jailhouse_pci_device *dev =
		jailhouse_cell_pci_devices(cell->config);
	void *reg_base = dmar_reg_base;
	int n, err;

	// HACK for QEMU
	if (dmar_units == 0)
		return 0;

	if (cell->id >= dmar_num_did)
		return -ERANGE;

	cell->vtd.page_table = page_alloc(&mem_pool, 1);
	if (!cell->vtd.page_table)
		return -ENOMEM;

	for (n = 0; n < config->num_memory_regions; n++, mem++) {
		err = vtd_map_memory_region(cell, mem);
		if (err)
			/* FIXME: release vtd.page_table */
			return err;
	}

	for (n = 0; n < config->num_pci_devices; n++)
		if (!vtd_add_device_to_cell(cell, &dev[n]))
			/* FIXME: release vtd.page_table,
			 * revert device additions*/
			return -ENOMEM;

	if (!(mmio_read32(reg_base + VTD_GSTS_REG) & VTD_GSTS_TES))
		for (n = 0; n < dmar_units; n++, reg_base += PAGE_SIZE) {
			mmio_write64(reg_base + VTD_RTADDR_REG,
				     page_map_hvirt2phys(root_entry_table));
			mmio_write32(reg_base + VTD_GCMD_REG, VTD_GCMD_SRTP);
			while (!(mmio_read32(reg_base + VTD_GSTS_REG) &
				 VTD_GSTS_SRTP))
				cpu_relax();

			vtd_flush_dmar_caches(reg_base, VTD_CCMD_CIRG_GLOBAL,
					      VTD_IOTLB_IIRG_GLOBAL);

			mmio_write32(reg_base + VTD_GCMD_REG, VTD_GCMD_TE);
			while (!(mmio_read32(reg_base + VTD_GSTS_REG) &
				 VTD_GSTS_TES))
				cpu_relax();
		}

	return 0;
}

static void
vtd_remove_device_from_cell(struct cell *cell,
			    const struct jailhouse_pci_device *device)
{
	u64 root_entry_lo = root_entry_table[device->bus].lo_word;
	struct vtd_entry *context_entry_table =
		page_map_phys2hvirt(root_entry_lo & PAGE_MASK);
	struct vtd_entry *context_entry = &context_entry_table[device->devfn];
	unsigned int n;

	if (!(context_entry->lo_word & VTD_CTX_PRESENT))
		return;

	printk("Removing PCI device %02x:%02x.%x from cell \"%s\"\n",
	       device->bus, device->devfn >> 3, device->devfn & 7,
	       cell->config->name);

	context_entry->lo_word &= ~VTD_CTX_PRESENT;
	flush_cache(&context_entry->lo_word, sizeof(u64));

	for (n = 0; n < 256; n++)
		if (context_entry_table[n].lo_word & VTD_CTX_PRESENT)
			return;

	root_entry_table[device->bus].lo_word &= ~VTD_ROOT_PRESENT;
	flush_cache(&root_entry_table[device->bus].lo_word, sizeof(u64));
	page_free(&mem_pool, context_entry_table, 1);
}

void vtd_linux_cell_shrink(struct jailhouse_cell_desc *config)
{
	const struct jailhouse_memory *mem =
		jailhouse_cell_mem_regions(config);
	const struct jailhouse_pci_device *dev =
		jailhouse_cell_pci_devices(config);
	unsigned int n;

	for (n = 0; n < config->num_memory_regions; n++, mem++)
		if (mem->access_flags & JAILHOUSE_MEM_DMA)
			page_map_destroy(linux_cell.vtd.page_table,
					 mem->phys_start, mem->size,
					 dmar_pt_levels, PAGE_MAP_COHERENT);

	for (n = 0; n < config->num_pci_devices; n++)
		vtd_remove_device_from_cell(&linux_cell, &dev[n]);

	vtd_flush_domain_caches(linux_cell.id);
}

int vtd_map_memory_region(struct cell *cell,
			  const struct jailhouse_memory *mem)
{
	u32 page_flags = 0;

	// HACK for QEMU
	if (dmar_units == 0)
		return 0;

	if (!(mem->access_flags & JAILHOUSE_MEM_DMA))
		return 0;

	if (mem->access_flags & JAILHOUSE_MEM_READ)
		page_flags |= VTD_PAGE_READ;
	if (mem->access_flags & JAILHOUSE_MEM_WRITE)
		page_flags |= VTD_PAGE_WRITE;

	return page_map_create(cell->vtd.page_table, mem->phys_start,
			       mem->size, mem->virt_start, page_flags,
			       VTD_PAGE_READ | VTD_PAGE_WRITE,
			       dmar_pt_levels, PAGE_MAP_COHERENT);
}

void vtd_unmap_memory_region(struct cell *cell,
			     const struct jailhouse_memory *mem)
{
	// HACK for QEMU
	if (dmar_units == 0)
		return;

	if (mem->access_flags & JAILHOUSE_MEM_DMA)
		page_map_destroy(cell->vtd.page_table, mem->virt_start,
				 mem->size, dmar_pt_levels, PAGE_MAP_COHERENT);
}

static bool vtd_return_device_to_linux(const struct jailhouse_pci_device *dev)
{
	const struct jailhouse_pci_device *linux_dev =
		jailhouse_cell_pci_devices(linux_cell.config);
	unsigned int n;

	for (n = 0; n < linux_cell.config->num_pci_devices; n++)
		if (linux_dev[n].domain == dev->domain &&
		    linux_dev[n].bus == dev->bus &&
		    linux_dev[n].devfn == dev->devfn)
			return vtd_add_device_to_cell(&linux_cell,
						      &linux_dev[n]);
	return true;
}

void vtd_cell_exit(struct cell *cell)
{
	const struct jailhouse_pci_device *dev =
		jailhouse_cell_pci_devices(cell->config);
	unsigned int n;

	for (n = 0; n < cell->config->num_pci_devices; n++) {
		vtd_remove_device_from_cell(cell, &dev[n]);
		if (!vtd_return_device_to_linux(&dev[n]))
			printk("WARNING: Failed to re-assign PCI device to "
			       "Linux cell\n");
	}

	vtd_flush_domain_caches(cell->id);
	vtd_flush_domain_caches(linux_cell.id);

	page_free(&mem_pool, cell->vtd.page_table, 1);
}

void vtd_shutdown(void)
{
	void *reg_base = dmar_reg_base;
	unsigned int n;

	for (n = 0; n < dmar_units; n++, reg_base += PAGE_SIZE) {
		mmio_write32(reg_base + VTD_GCMD_REG, 0);
		while (mmio_read32(reg_base + VTD_GSTS_REG) & VTD_GSTS_TES)
			cpu_relax();
	}
}
