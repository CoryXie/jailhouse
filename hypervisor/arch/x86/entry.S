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

#include <asm/percpu.h>

/* Entry point for Linux loader module on JAILHOUSE_ENABLE */
	.text
	.globl arch_entry
arch_entry:
	cli

	push %rbp
	push %rbx
	push %r12
	push %r13
	push %r14
	push %r15

	mov %rdi,%rdx
	shl $PERCPU_SIZE_SHIFT,%rdi
	lea __page_pool(%rip),%rax
	add %rax,%rdi

	mov %rsp,PERCPU_LINUX_SP(%rdi)
	mov %edx,PERCPU_CPU_ID(%rdi)

	lea PERCPU_STACK_END-8(%rdi),%rsp

	push %rdi

	call entry

	pop %rdi

	mov PERCPU_LINUX_SP(%rdi),%rsp

	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %rbx
	pop %rbp

	ret


/* Fix up Global Offset Table with absolute hypervisor address */
	.globl got_init
got_init:
	lea __got_start(%rip),%rdx
	lea __got_end(%rip),%rcx
	lea hypervisor_header(%rip),%rax

got_loop:
	cmp %rdx,%rcx
	je got_done

	add %rax,(%rdx)
	add $8,%rdx
	jmp got_loop

got_done:
	ret


/* Exception/interrupt entry points */
.macro no_error_entry vector
	.balign 16
	pushq $-1
	pushq $\vector
	mov %rsp,%rdi
	call exception_handler
1:	jmp 1b
.endm

.macro error_entry vector
	.balign 16
	pushq $\vector
	mov %rsp,%rdi
	call exception_handler
1:	jmp 1b
.endm

	.global exception_entries
	.balign 16
exception_entries:
	no_error_entry 0
	no_error_entry 1
vector=3
.rept 5
	no_error_entry vector
	vector=vector+1
.endr
	error_entry 8
	no_error_entry 9
vector=10
.rept 5
	error_entry vector
	vector=vector+1
.endr
	no_error_entry 16
	error_entry 17
	no_error_entry 18
	no_error_entry 19


.macro interrupt_entry func
	push %rax
	push %rcx
	push %rdx
	push %rsi
	push %rdi
	push %r8
	push %r9
	push %r10
	push %r11

	mov %rsp,%rdi
	and $PAGE_MASK,%rdi
	call \func

	pop %r11
	pop %r10
	pop %r9
	pop %r8
	pop %rdi
	pop %rsi
	pop %rdx
	pop %rcx
	pop %rax

	iretq
.endm

	.global nmi_entry
	.balign 16
nmi_entry:
	interrupt_entry apic_nmi_handler

	.global irq_entry
	.balign 16
irq_entry:
	interrupt_entry apic_irq_handler


/* VM-exit handling */
	.globl vm_exit
vm_exit:
	push %rax
	push %rcx
	push %rdx
	push %rbx
	sub $8,%rsp /* placeholder for rsp */
	push %rbp
	push %rsi
	push %rdi
	push %r8
	push %r9
	push %r10
	push %r11
	push %r12
	push %r13
	push %r14
	push %r15

	mov %rsp,%rdi
	lea -PERCPU_STACK_END+16*8(%rsp),%rsi
	call vmx_handle_exit

	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %r11
	pop %r10
	pop %r9
	pop %r8
	pop %rdi
	pop %rsi
	pop %rbp
	add $8,%rsp
	pop %rbx
	pop %rdx
	pop %rcx
	pop %rax

	vmresume

	lea -PERCPU_STACK_END(%rsp),%rdi
	jmp vmx_entry_failure
