/*
 *  linux/arch/arm/kernel/setup.c
 *
 *  Copyright (C) 1995-2001 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/utsname.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/bootmem.h>
#include <linux/seq_file.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/root_dev.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>

#include <asm/unified.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/elf.h>
#include <asm/procinfo.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/mach-types.h>
#include <asm/cacheflush.h>
#include <asm/cachetype.h>
#include <asm/tlbflush.h>

#ifdef CONFIG_ARM_OF
#include <asm/prom.h>
#include <plat/board-mapphone.h>
#endif

#include <asm/mach/arch.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>
#include <asm/traps.h>
#include <asm/unwind.h>
#ifdef CONFIG_BOOTINFO
#include <asm/bootinfo.h>
#endif

#include "compat.h"
#include "atags.h"
#include "tcm.h"

#ifndef MEM_SIZE
#define MEM_SIZE	(16*1024*1024)
#endif

#if defined(CONFIG_FPE_NWFPE) || defined(CONFIG_FPE_FASTFPE)
char fpe_type[8];

static int __init fpe_setup(char *line)
{
	memcpy(fpe_type, line, 8);
	return 1;
}

__setup("fpe=", fpe_setup);
#endif

extern void paging_init(struct machine_desc *desc);
extern void reboot_setup(char *str);

unsigned int processor_id;
EXPORT_SYMBOL(processor_id);
unsigned int __machine_arch_type;
EXPORT_SYMBOL(__machine_arch_type);
unsigned int cacheid;
EXPORT_SYMBOL(cacheid);

unsigned int __atags_pointer __initdata;

unsigned int system_rev;
EXPORT_SYMBOL(system_rev);

unsigned int system_serial_low;
EXPORT_SYMBOL(system_serial_low);

unsigned int system_serial_high;
EXPORT_SYMBOL(system_serial_high);

unsigned int elf_hwcap;
EXPORT_SYMBOL(elf_hwcap);


#ifdef MULTI_CPU
struct processor processor;
#endif
#ifdef MULTI_TLB
struct cpu_tlb_fns cpu_tlb;
#endif
#ifdef MULTI_USER
struct cpu_user_fns cpu_user;
#endif
#ifdef MULTI_CACHE
struct cpu_cache_fns cpu_cache;
#endif
#ifdef CONFIG_OUTER_CACHE
struct outer_cache_fns outer_cache;
EXPORT_SYMBOL(outer_cache);
#endif

struct stack {
	u32 irq[3];
	u32 abt[3];
	u32 und[3];
} ____cacheline_aligned;

static struct stack stacks[NR_CPUS];

char elf_platform[ELF_PLATFORM_SIZE];
EXPORT_SYMBOL(elf_platform);

static const char *cpu_name;
static const char *machine_name;
static char *cpu_tier;
static char __initdata command_line[COMMAND_LINE_SIZE];

static char default_command_line[COMMAND_LINE_SIZE] __initdata = CONFIG_CMDLINE;
static union { char c[4]; unsigned long l; } endian_test __initdata = { { 'l', '?', '?', 'b' } };
#define ENDIANNESS ((char)endian_test.l)

DEFINE_PER_CPU(struct cpuinfo_arm, cpu_data);

/*
 * Standard memory resources
 */
static struct resource mem_res[] = {
	{
		.name = "Video RAM",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	},
	{
		.name = "Kernel text",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	}
};

#define video_ram   mem_res[0]
#define kernel_code mem_res[1]
#define kernel_data mem_res[2]

static struct resource io_res[] = {
	{
		.name = "reserved",
		.start = 0x3bc,
		.end = 0x3be,
		.flags = IORESOURCE_IO | IORESOURCE_BUSY
	},
	{
		.name = "reserved",
		.start = 0x378,
		.end = 0x37f,
		.flags = IORESOURCE_IO | IORESOURCE_BUSY
	},
	{
		.name = "reserved",
		.start = 0x278,
		.end = 0x27f,
		.flags = IORESOURCE_IO | IORESOURCE_BUSY
	}
};

#define lp0 io_res[0]
#define lp1 io_res[1]
#define lp2 io_res[2]

static const char *proc_arch[] = {
	"undefined/unknown",
	"3",
	"4",
	"4T",
	"5",
	"5T",
	"5TE",
	"5TEJ",
	"6TEJ",
	"7",
	"?(11)",
	"?(12)",
	"?(13)",
	"?(14)",
	"?(15)",
	"?(16)",
	"?(17)",
};

int cpu_architecture(void)
{
	int cpu_arch;

	if ((read_cpuid_id() & 0x0008f000) == 0) {
		cpu_arch = CPU_ARCH_UNKNOWN;
	} else if ((read_cpuid_id() & 0x0008f000) == 0x00007000) {
		cpu_arch = (read_cpuid_id() & (1 << 23)) ? CPU_ARCH_ARMv4T : CPU_ARCH_ARMv3;
	} else if ((read_cpuid_id() & 0x00080000) == 0x00000000) {
		cpu_arch = (read_cpuid_id() >> 16) & 7;
		if (cpu_arch)
			cpu_arch += CPU_ARCH_ARMv3;
	} else if ((read_cpuid_id() & 0x000f0000) == 0x000f0000) {
		unsigned int mmfr0;

		/* Revised CPUID format. Read the Memory Model Feature
		 * Register 0 and check for VMSAv7 or PMSAv7 */
		asm("mrc	p15, 0, %0, c0, c1, 4"
		    : "=r" (mmfr0));
		if ((mmfr0 & 0x0000000f) == 0x00000003 ||
		    (mmfr0 & 0x000000f0) == 0x00000030)
			cpu_arch = CPU_ARCH_ARMv7;
		else if ((mmfr0 & 0x0000000f) == 0x00000002 ||
			 (mmfr0 & 0x000000f0) == 0x00000020)
			cpu_arch = CPU_ARCH_ARMv6;
		else
			cpu_arch = CPU_ARCH_UNKNOWN;
	} else
		cpu_arch = CPU_ARCH_UNKNOWN;

	return cpu_arch;
}

static int cpu_has_aliasing_icache(unsigned int arch)
{
	int aliasing_icache;
	unsigned int id_reg, num_sets, line_size;

	/* arch specifies the register format */
	switch (arch) {
	case CPU_ARCH_ARMv7:
		asm("mcr	p15, 2, %1, c0, c0, 0	@ set CSSELR\n"
		    "isb\n"
		    "mrc	p15, 1, %0, c0, c0, 0	@ read CCSIDR"
		    : "=r" (id_reg)
		    : "r" (1));
		line_size = 4 << ((id_reg & 0x7) + 2);
		num_sets = ((id_reg >> 13) & 0x7fff) + 1;
		aliasing_icache = (line_size * num_sets) > PAGE_SIZE;
		break;
	case CPU_ARCH_ARMv6:
		aliasing_icache = read_cpuid_cachetype() & (1 << 11);
		break;
	default:
		/* I-cache aliases will be handled by D-cache aliasing code */
		aliasing_icache = 0;
	}

	return aliasing_icache;
}

static void __init cacheid_init(void)
{
	unsigned int cachetype = read_cpuid_cachetype();
	unsigned int arch = cpu_architecture();

	if (arch >= CPU_ARCH_ARMv6) {
		if ((cachetype & (7 << 29)) == 4 << 29) {
			/* ARMv7 register format */
			cacheid = CACHEID_VIPT_NONALIASING;
			if ((cachetype & (3 << 14)) == 1 << 14)
				cacheid |= CACHEID_ASID_TAGGED;
			else if (cpu_has_aliasing_icache(CPU_ARCH_ARMv7))
				cacheid |= CACHEID_VIPT_I_ALIASING;
		} else if (cachetype & (1 << 23)) {
			cacheid = CACHEID_VIPT_ALIASING;
		} else {
			cacheid = CACHEID_VIPT_NONALIASING;
			if (cpu_has_aliasing_icache(CPU_ARCH_ARMv6))
				cacheid |= CACHEID_VIPT_I_ALIASING;
		}
	} else {
		cacheid = CACHEID_VIVT;
	}

	printk("CPU: %s data cache, %s instruction cache\n",
		cache_is_vivt() ? "VIVT" :
		cache_is_vipt_aliasing() ? "VIPT aliasing" :
		cache_is_vipt_nonaliasing() ? "VIPT nonaliasing" : "unknown",
		cache_is_vivt() ? "VIVT" :
		icache_is_vivt_asid_tagged() ? "VIVT ASID tagged" :
		icache_is_vipt_aliasing() ? "VIPT aliasing" :
		cache_is_vipt_nonaliasing() ? "VIPT nonaliasing" : "unknown");
}

/*
 * These functions re-use the assembly code in head.S, which
 * already provide the required functionality.
 */
extern struct proc_info_list *lookup_processor_type(unsigned int);
extern struct machine_desc *lookup_machine_type(unsigned int);

static void __init setup_processor(void)
{
	struct proc_info_list *list;

	/*
	 * locate processor in the list of supported processor
	 * types.  The linker builds this table for us from the
	 * entries in arch/arm/mm/proc-*.S
	 */
	list = lookup_processor_type(read_cpuid_id());
	if (!list) {
		printk("CPU configuration botched (ID %08x), unable "
		       "to continue.\n", read_cpuid_id());
		while (1);
	}

	cpu_name = list->cpu_name;

#ifdef MULTI_CPU
	processor = *list->proc;
#endif
#ifdef MULTI_TLB
	cpu_tlb = *list->tlb;
#endif
#ifdef MULTI_USER
	cpu_user = *list->user;
#endif
#ifdef MULTI_CACHE
	cpu_cache = *list->cache;
#endif

	printk("CPU: %s [%08x] revision %d (ARMv%s), cr=%08lx\n",
	       cpu_name, read_cpuid_id(), read_cpuid_id() & 15,
	       proc_arch[cpu_architecture()], cr_alignment);

	sprintf(init_utsname()->machine, "%s%c", list->arch_name, ENDIANNESS);
	sprintf(elf_platform, "%s%c", list->elf_name, ENDIANNESS);
	elf_hwcap = list->elf_hwcap;
#ifndef CONFIG_ARM_THUMB
	elf_hwcap &= ~HWCAP_THUMB;
#endif

	cacheid_init();
	cpu_proc_init();
}

/*
 * cpu_init - initialise one CPU.
 *
 * cpu_init sets up the per-CPU stacks.
 */
void cpu_init(void)
{
	unsigned int cpu = smp_processor_id();
	struct stack *stk = &stacks[cpu];

	if (cpu >= NR_CPUS) {
		printk(KERN_CRIT "CPU%u: bad primary CPU number\n", cpu);
		BUG();
	}

	/*
	 * Define the placement constraint for the inline asm directive below.
	 * In Thumb-2, msr with an immediate value is not allowed.
	 */
#ifdef CONFIG_THUMB2_KERNEL
#define PLC	"r"
#else
#define PLC	"I"
#endif

	/*
	 * setup stacks for re-entrant exception handlers
	 */
	__asm__ (
	"msr	cpsr_c, %1\n\t"
	"add	r14, %0, %2\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %3\n\t"
	"add	r14, %0, %4\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %5\n\t"
	"add	r14, %0, %6\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %7"
	    :
	    : "r" (stk),
	      PLC (PSR_F_BIT | PSR_I_BIT | IRQ_MODE),
	      "I" (offsetof(struct stack, irq[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | ABT_MODE),
	      "I" (offsetof(struct stack, abt[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | UND_MODE),
	      "I" (offsetof(struct stack, und[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | SVC_MODE)
	    : "r14");
}

static struct machine_desc * __init setup_machine(unsigned int nr)
{
	struct machine_desc *list;

	/*
	 * locate machine in the list of supported machines.
	 */
	list = lookup_machine_type(nr);
	if (!list) {
		printk("Machine configuration botched (nr %d), unable "
		       "to continue.\n", nr);
		while (1);
	}

	printk("Machine: %s\n", list->name);

	return list;
}

static int __init arm_add_memory(unsigned long start, unsigned long size)
{
	struct membank *bank = &meminfo.bank[meminfo.nr_banks];

	if (meminfo.nr_banks >= NR_BANKS) {
		printk(KERN_CRIT "NR_BANKS too low, "
			"ignoring memory at %#lx\n", start);
		return -EINVAL;
	}

	/*
	 * Ensure that start/size are aligned to a page boundary.
	 * Size is appropriately rounded down, start is rounded up.
	 */
	size -= start & ~PAGE_MASK;
	bank->start = PAGE_ALIGN(start);
	bank->size  = size & PAGE_MASK;

	/*
	 * Check whether this memory region has non-zero size or
	 * invalid node number.
	 */
	if (bank->size == 0)
		return -EINVAL;

	meminfo.nr_banks++;
	return 0;
}

/*
 * Pick out the memory size.  We look for mem=size@start,
 * where start and size are "size[KkMm]"
 */
static int __init early_mem(char *p)
{
	static int usermem __initdata = 0;
	unsigned long size, start;
	char *endp;

	/*
	 * If the user specifies memory size, we
	 * blow away any automatically generated
	 * size.
	 */
	if (usermem == 0) {
		usermem = 1;
		meminfo.nr_banks = 0;
	}

	start = PHYS_OFFSET;
	size  = memparse(p, &endp);
	if (*endp == '@')
		start = memparse(endp + 1, NULL);

	arm_add_memory(start, size);

	return 0;
}
early_param("mem", early_mem);

static void __init
setup_ramdisk(int doload, int prompt, int image_start, unsigned int rd_sz)
{
#ifdef CONFIG_BLK_DEV_RAM
	extern int rd_size, rd_image_start, rd_prompt, rd_doload;

	rd_image_start = image_start;
	rd_prompt = prompt;
	rd_doload = doload;

	if (rd_sz)
		rd_size = rd_sz;
#endif
}

static void __init
request_standard_resources(struct meminfo *mi, struct machine_desc *mdesc)
{
	struct resource *res;
	int i;

	kernel_code.start   = virt_to_phys(_text);
	kernel_code.end     = virt_to_phys(_etext - 1);
	kernel_data.start   = virt_to_phys(_sdata);
	kernel_data.end     = virt_to_phys(_end - 1);

	for (i = 0; i < mi->nr_banks; i++) {
		if (mi->bank[i].size == 0)
			continue;

		res = alloc_bootmem_low(sizeof(*res));
		res->name  = "System RAM";
		res->start = mi->bank[i].start;
		res->end   = mi->bank[i].start + mi->bank[i].size - 1;
		res->flags = IORESOURCE_MEM | IORESOURCE_BUSY;

		request_resource(&iomem_resource, res);

		if (kernel_code.start >= res->start &&
		    kernel_code.end <= res->end)
			request_resource(res, &kernel_code);
		if (kernel_data.start >= res->start &&
		    kernel_data.end <= res->end)
			request_resource(res, &kernel_data);
	}

	if (mdesc->video_start) {
		video_ram.start = mdesc->video_start;
		video_ram.end   = mdesc->video_end;
		request_resource(&iomem_resource, &video_ram);
	}

	/*
	 * Some machines don't have the possibility of ever
	 * possessing lp0, lp1 or lp2
	 */
	if (mdesc->reserve_lp0)
		request_resource(&ioport_resource, &lp0);
	if (mdesc->reserve_lp1)
		request_resource(&ioport_resource, &lp1);
	if (mdesc->reserve_lp2)
		request_resource(&ioport_resource, &lp2);
}

/*
 *  Tag parsing.
 *
 * This is the new way of passing data to the kernel at boot time.  Rather
 * than passing a fixed inflexible structure to the kernel, we pass a list
 * of variable-sized tags to the kernel.  The first tag must be a ATAG_CORE
 * tag for the list to be recognised (to distinguish the tagged list from
 * a param_struct).  The list is terminated with a zero-length tag (this tag
 * is not parsed in any way).
 */
static int __init parse_tag_core(const struct tag *tag)
{
	if (tag->hdr.size > 2) {
		if ((tag->u.core.flags & 1) == 0)
			root_mountflags &= ~MS_RDONLY;
		ROOT_DEV = old_decode_dev(tag->u.core.rootdev);
	}
	return 0;
}

__tagtable(ATAG_CORE, parse_tag_core);

static int __init parse_tag_mem32(const struct tag *tag)
{
	return arm_add_memory(tag->u.mem.start, tag->u.mem.size);
}

__tagtable(ATAG_MEM, parse_tag_mem32);

#if defined(CONFIG_VGA_CONSOLE) || defined(CONFIG_DUMMY_CONSOLE)
struct screen_info screen_info = {
 .orig_video_lines	= 30,
 .orig_video_cols	= 80,
 .orig_video_mode	= 0,
 .orig_video_ega_bx	= 0,
 .orig_video_isVGA	= 1,
 .orig_video_points	= 8
};

static int __init parse_tag_videotext(const struct tag *tag)
{
	screen_info.orig_x            = tag->u.videotext.x;
	screen_info.orig_y            = tag->u.videotext.y;
	screen_info.orig_video_page   = tag->u.videotext.video_page;
	screen_info.orig_video_mode   = tag->u.videotext.video_mode;
	screen_info.orig_video_cols   = tag->u.videotext.video_cols;
	screen_info.orig_video_ega_bx = tag->u.videotext.video_ega_bx;
	screen_info.orig_video_lines  = tag->u.videotext.video_lines;
	screen_info.orig_video_isVGA  = tag->u.videotext.video_isvga;
	screen_info.orig_video_points = tag->u.videotext.video_points;
	return 0;
}

__tagtable(ATAG_VIDEOTEXT, parse_tag_videotext);
#endif

static int __init parse_tag_ramdisk(const struct tag *tag)
{
	setup_ramdisk((tag->u.ramdisk.flags & 1) == 0,
		      (tag->u.ramdisk.flags & 2) == 0,
		      tag->u.ramdisk.start, tag->u.ramdisk.size);
	return 0;
}

__tagtable(ATAG_RAMDISK, parse_tag_ramdisk);

static int __init parse_tag_serialnr(const struct tag *tag)
{
	system_serial_low = tag->u.serialnr.low;
	system_serial_high = tag->u.serialnr.high;
	return 0;
}

__tagtable(ATAG_SERIAL, parse_tag_serialnr);

static int __init parse_tag_revision(const struct tag *tag)
{
	system_rev = tag->u.revision.rev;
	return 0;
}

__tagtable(ATAG_REVISION, parse_tag_revision);

static int __init parse_tag_cmdline(const struct tag *tag)
{
	strlcpy(default_command_line, tag->u.cmdline.cmdline, COMMAND_LINE_SIZE);
	return 0;
}

__tagtable(ATAG_CMDLINE, parse_tag_cmdline);

#ifdef CONFIG_BOOTINFO

static int __init parse_tag_powerup_reason(const struct tag *tag)
{
	bi_set_powerup_reason(tag->u.powerup_reason.powerup_reason);
	printk(KERN_WARNING "%s: powerup reason=0x%08x\n",
				__func__, bi_powerup_reason());
	return 0;
}

__tagtable(ATAG_POWERUP_REASON, parse_tag_powerup_reason);

static int __init parse_tag_mbm_version(const struct tag *tag)
{
	bi_set_mbm_version(tag->u.mbm_version.mbm_version);
	printk(KERN_INFO "%s: mbm_version=0x%08x\n",
				__func__, bi_mbm_version());
	return 0;
}

__tagtable(ATAG_MBM_VERSION, parse_tag_mbm_version);

static int __init parse_tag_mbm_loader_version(const struct tag *tag)
{
	bi_set_mbm_loader_version(tag->
			u.mbm_loader_version.mbm_loader_version);
	printk(KERN_INFO "%s: mbm_loader_version=0x%08x\n",
				__func__, bi_mbm_loader_version());
	return 0;
}

__tagtable(ATAG_MBM_LOADER_VERSION, parse_tag_mbm_loader_version);

static int __init parse_tag_battery_status_at_boot(const struct tag *tag)
{
	bi_set_battery_status_at_boot(tag->
			u.battery_status_at_boot.battery_status_at_boot);
	printk(KERN_INFO "%s: battery_status_at_boot=0x%08x\n",
				__func__, bi_battery_status_at_boot());
	return 0;
}

__tagtable(ATAG_BATTERY_STATUS_AT_BOOT, parse_tag_battery_status_at_boot);

static int __init parse_tag_cid_recover_boot(const struct tag *tag)
{
	bi_set_cid_recover_boot(tag->u.cid_recover_boot.cid_recover_boot);
	printk(KERN_INFO "%s: cid_recover_boot=\"%d\"\n",
				__func__, bi_cid_recover_boot());
	return 0;
}

__tagtable(ATAG_CID_RECOVER_BOOT, parse_tag_cid_recover_boot);

#endif /* CONFIG_BOOTINFO */

/*
 * Scan the tag table for this tag, and call its parse function.
 * The tag table is built by the linker from all the __tagtable
 * declarations.
 */
static int __init parse_tag(const struct tag *tag)
{
	extern struct tagtable __tagtable_begin, __tagtable_end;
	struct tagtable *t;

	for (t = &__tagtable_begin; t < &__tagtable_end; t++)
		if (tag->hdr.tag == t->tag) {
			t->parse(tag);
			break;
		}

	return t < &__tagtable_end;
}

/*
 * Parse all tags in the list, checking both the global and architecture
 * specific tag tables.
 */
static void __init parse_tags(const struct tag *t)
{
	for (; t->hdr.size; t = tag_next(t))
		if (!parse_tag(t))
			printk(KERN_WARNING
				"Ignoring unrecognised tag 0x%08x\n",
				t->hdr.tag);
}

/*
 * This holds our defaults.
 */
static struct init_tags {
	struct tag_header hdr1;
	struct tag_core   core;
	struct tag_header hdr2;
	struct tag_mem32  mem;
	struct tag_header hdr3;
} init_tags __initdata = {
	{ tag_size(tag_core), ATAG_CORE },
	{ 1, PAGE_SIZE, 0xff },
	{ tag_size(tag_mem32), ATAG_MEM },
	{ MEM_SIZE, PHYS_OFFSET },
	{ 0, ATAG_NONE }
};

static void (*init_machine)(void) __initdata;

static int __init customize_machine(void)
{
	/* customizes platform devices, or adds new ones */
	if (init_machine)
		init_machine();
	return 0;
}
arch_initcall(customize_machine);

void __init setup_arch(char **cmdline_p)
{
	struct tag *tags = (struct tag *)&init_tags;
	struct machine_desc *mdesc;
	char *from = default_command_line;

	unwind_init();

	setup_processor();
	mdesc = setup_machine(machine_arch_type);
	machine_name = mdesc->name;

	if (mdesc->soft_reboot)
		reboot_setup("s");

	if (__atags_pointer)
		tags = phys_to_virt(__atags_pointer);
	else if (mdesc->boot_params)
		tags = phys_to_virt(mdesc->boot_params);

	/*
	 * If we have the old style parameters, convert them to
	 * a tag list.
	 */
	if (tags->hdr.tag != ATAG_CORE)
		convert_to_tag_list(tags);
	if (tags->hdr.tag != ATAG_CORE)
		tags = (struct tag *)&init_tags;

	if (mdesc->fixup)
		mdesc->fixup(mdesc, tags, &from, &meminfo);

	if (tags->hdr.tag == ATAG_CORE) {
		if (meminfo.nr_banks != 0)
			squash_mem_tags(tags);
		save_atags(tags);
		parse_tags(tags);
	}

	init_mm.start_code = (unsigned long) _text;
	init_mm.end_code   = (unsigned long) _etext;
	init_mm.end_data   = (unsigned long) _edata;
	init_mm.brk	   = (unsigned long) _end;

	/* parse_early_param needs a boot_command_line */
	strlcpy(boot_command_line, from, COMMAND_LINE_SIZE);

	/* populate command_line too for later use, preserving boot_command_line */
	strlcpy(command_line, boot_command_line, COMMAND_LINE_SIZE);
	*cmdline_p = command_line;

	parse_early_param();

	arm_memblock_init(&meminfo);

	paging_init(mdesc);
#ifdef CONFIG_OF
	unflatten_device_tree();
#define DT_PATH_MACHINE		"/Machine@0"
#define DT_PROP_MACHINE_TYPE	"machine_type"
#define DT_PROP_CPU_TIER	"cpu_tier"
	{
		struct device_node *machine_node;
		const void *machine_prop;
		const void *cpu_tier_prop;

		cpu_tier = NULL;
		machine_node = of_find_node_by_path(DT_PATH_MACHINE);
		if (machine_node) {
			machine_prop = of_get_property(machine_node, \
				DT_PROP_MACHINE_TYPE, NULL);
			if (machine_prop)
				machine_name = (char *)machine_prop;

			cpu_tier_prop = of_get_property(machine_node, \
				DT_PROP_CPU_TIER, NULL);
			if (cpu_tier_prop)
				cpu_tier = (char *)cpu_tier_prop;

			of_node_put(machine_node);
		}
	}
#endif
	request_standard_resources(&meminfo, mdesc);

#ifdef CONFIG_SMP
	smp_init_cpus();
#endif

	cpu_init();
	tcm_init();

	/*
	 * Set up various architecture-specific pointers
	 */
	init_arch_irq = mdesc->init_irq;
	system_timer = mdesc->timer;
	init_machine = mdesc->init_machine;

#ifdef CONFIG_VT
#if defined(CONFIG_VGA_CONSOLE)
	conswitchp = &vga_con;
#elif defined(CONFIG_DUMMY_CONSOLE)
	conswitchp = &dummy_con;
#endif
#endif
	early_trap_init();
}


static int __init topology_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cpuinfo_arm *cpuinfo = &per_cpu(cpu_data, cpu);
		cpuinfo->cpu.hotpluggable = 1;
		register_cpu(&cpuinfo->cpu, cpu);
	}

	return 0;
}
subsys_initcall(topology_init);

#ifdef CONFIG_HAVE_PROC_CPU
static int __init proc_cpu_init(void)
{
	struct proc_dir_entry *res;

	res = proc_mkdir("cpu", NULL);
	if (!res)
		return -ENOMEM;
	return 0;
}
fs_initcall(proc_cpu_init);
#endif

static const char *hwcap_str[] = {
	"swp",
	"half",
	"thumb",
	"26bit",
	"fastmult",
	"fpa",
	"vfp",
	"edsp",
	"java",
	"iwmmxt",
	"crunch",
	"thumbee",
	"neon",
	"vfpv3",
	"vfpv3d16",
	NULL
};

static int c_show(struct seq_file *m, void *v)
{
	int i;
#if defined(CONFIG_ARM_OF)
	static char *p = NULL;
	int len = strlen(bp_model);

	if (!p && len) {
		p = kmalloc(strlen(machine_name) + len + 1, GFP_KERNEL);
		if (p) {
			*p = '\0';
			strcat(p, machine_name);
			machine_name = strcat(p, bp_model);
		}
	}
#endif
	seq_printf(m, "Processor\t: %s rev %d (%s)\n",
		   cpu_name, read_cpuid_id() & 15, elf_platform);

#if defined(CONFIG_SMP)
	for_each_online_cpu(i) {
		/*
		 * glibc reads /proc/cpuinfo to determine the number of
		 * online processors, looking for lines beginning with
		 * "processor".  Give glibc what it expects.
		 */
		seq_printf(m, "processor\t: %d\n", i);
		seq_printf(m, "BogoMIPS\t: %lu.%02lu\n\n",
			   per_cpu(cpu_data, i).loops_per_jiffy / (500000UL/HZ),
			   (per_cpu(cpu_data, i).loops_per_jiffy / (5000UL/HZ)) % 100);
	}
#else /* CONFIG_SMP */
	seq_printf(m, "BogoMIPS\t: %lu.%02lu\n",
		   loops_per_jiffy / (500000/HZ),
		   (loops_per_jiffy / (5000/HZ)) % 100);
#endif

	/* dump out the processor features */
	seq_puts(m, "Features\t: ");

	for (i = 0; hwcap_str[i]; i++)
		if (elf_hwcap & (1 << i))
			seq_printf(m, "%s ", hwcap_str[i]);

	seq_printf(m, "\nCPU implementer\t: 0x%02x\n", read_cpuid_id() >> 24);
	seq_printf(m, "CPU architecture: %s\n", proc_arch[cpu_architecture()]);

	if ((read_cpuid_id() & 0x0008f000) == 0x00000000) {
		/* pre-ARM7 */
		seq_printf(m, "CPU part\t: %07x\n", read_cpuid_id() >> 4);
	} else {
		if ((read_cpuid_id() & 0x0008f000) == 0x00007000) {
			/* ARM7 */
			seq_printf(m, "CPU variant\t: 0x%02x\n",
				   (read_cpuid_id() >> 16) & 127);
		} else {
			/* post-ARM7 */
			seq_printf(m, "CPU variant\t: 0x%x\n",
				   (read_cpuid_id() >> 20) & 15);
		}
		seq_printf(m, "CPU part\t: 0x%03x\n",
			   (read_cpuid_id() >> 4) & 0xfff);
	}
	seq_printf(m, "CPU revision\t: %d\n", read_cpuid_id() & 15);

	seq_puts(m, "\n");

	seq_printf(m, "Hardware\t: %s\n", machine_name);
	seq_printf(m, "Revision\t: %04x\n", system_rev);
	seq_printf(m, "Serial\t\t: %08x%08x\n",
		   system_serial_high, system_serial_low);
	if (cpu_tier)
		seq_printf(m, "CPU Tier\t: %s\n", cpu_tier);
	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= c_show
};
