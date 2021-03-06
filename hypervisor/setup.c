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

#include <jailhouse/processor.h>
#include <jailhouse/printk.h>
#include <jailhouse/entry.h>
#include <jailhouse/paging.h>
#include <jailhouse/control.h>
#include <jailhouse/string.h>
#include <asm/spinlock.h>

extern u8 __start[];
extern u8 __bss_start[], __bss_end[];

void *config_memory;

static DEFINE_SPINLOCK(init_lock);
static unsigned int master_cpu_id = -1;
static volatile unsigned int initialized_cpus;
static volatile int error;
struct cell linux_cell;

static int register_linux_cpu(struct per_cpu *cpu_data)
{
	const unsigned long *system_cpu_set =
		jailhouse_cell_cpu_set(&system_config->system);

	if (cpu_data->cpu_id >= system_config->system.cpu_set_size * 8 ||
	    !test_bit(cpu_data->cpu_id, system_cpu_set))
		return -EINVAL;

	cpu_data->cell = &linux_cell;
	set_bit(cpu_data->cpu_id, linux_cell.cpu_set->bitmap);
	return 0;
}

static void init_early(unsigned int cpu_id)
{
	unsigned long size;

	master_cpu_id = cpu_id;

	/* must be first, printk/arch_dbg_write uses the GOT */
	got_init();

	arch_dbg_write_init();

	printk("\nInitializing Jailhouse hypervisor on CPU %d\n", cpu_id);
	printk("Code location: %p\n",
	       __start + sizeof(struct jailhouse_header));

	error = paging_init();
	if (error)
		return;

	linux_cell.config = &system_config->system;

	if (system_config->config_memory.size > 0) {
		size = PAGE_ALIGN(system_config->config_memory.size);

		config_memory = page_alloc(&remap_pool, size / PAGE_SIZE);
		if (!config_memory) {
			error = -ENOMEM;
			return;
		}

		error = page_map_create(hv_page_table,
				system_config->config_memory.phys_start,
				size, (unsigned long)config_memory,
				PAGE_READONLY_FLAGS, PAGE_DEFAULT_FLAGS,
				PAGE_DIR_LEVELS, PAGE_MAP_NON_COHERENT);
		if (error)
			return;
	}

	error = check_mem_regions(&system_config->system);
	if (error)
		return;

	error = arch_init_early(&linux_cell);
	if (error)
		return;

	linux_cell.id = -1;
	error = cell_init(&linux_cell, false);
	if (error)
		return;

	page_map_dump_stats("after early setup");
	printk("Initializing first processor:\n");
}

static void cpu_init(struct per_cpu *cpu_data)
{
	int err;

	printk(" CPU %d... ", cpu_data->cpu_id);

	err = register_linux_cpu(cpu_data);
	if (err)
		goto failed;

	err = arch_cpu_init(cpu_data);
	if (err)
		goto failed;

	printk("OK\n");

	/* If this CPU is last, make sure everything was committed before we
	 * signal the other CPUs spinning on initialized_cpus that they can
	 * continue. */
	memory_barrier();
	initialized_cpus++;
	return;

failed:
	printk("FAILED\n");
	if (!error)
		error = err;
}

static void init_late(void)
{
	error = arch_init_late(&linux_cell);
	if (error)
		return;

	page_map_dump_stats("after late setup");
	printk("Initializing remaining processors:\n");
}

int entry(struct per_cpu *cpu_data)
{
	bool master = false;

	spin_lock(&init_lock);

	if (master_cpu_id == -1) {
		master = true;
		init_early(cpu_data->cpu_id);
	}

	if (!error) {
		cpu_init(cpu_data);

		if (master && !error)
			init_late();
	}

	spin_unlock(&init_lock);

	while (!error && initialized_cpus < hypervisor_header.online_cpus)
		cpu_relax();

	if (error) {
		arch_cpu_restore(cpu_data);
		return error;
	}

	if (master)
		printk("Activating hypervisor\n");

	/* point of no return */
	arch_cpu_activate_vmm(cpu_data);
}

struct jailhouse_header __attribute__((section(".header")))
hypervisor_header = {
	.signature = JAILHOUSE_SIGNATURE,
	.bss_start = (unsigned long)__bss_start,
	.bss_end = (unsigned long)__bss_end,
	.percpu_size = sizeof(struct per_cpu),
	.entry = (unsigned long)arch_entry,
};
