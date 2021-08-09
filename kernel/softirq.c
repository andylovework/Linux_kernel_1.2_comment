/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 * do_bottom_half() runs at normal kernel priority: all interrupts
 * enabled.  do_bottom_half() is atomic with respect to itself: a
 * bottom_half handler need not be re-entrant.
 */
 
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/kernel_stat.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/mm.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/bitops.h>

#define INCLUDE_INLINE_FUNCS
#include <linux/tqueue.h>

/*
	记录中断服务程序的嵌套层数。正常运行时，intr_count为0。
	当处理硬件中断、执行任务队列中的任务或者执行bottom half
	队列中的任务时，intr_count非0。这时，内核禁止某些操作，
	例如不允许重新调度。
*/
unsigned long intr_count = 0;

/*
			Bh机制 
	以前内核中的Bh机制设置了一个函数指针数组bh_base[]，它把所有的后半部分都组织起来，
	其大小为32，数组中的每一项就是一个后半部分，即一个bh 函数。同时，又设置了两个32
	位无符号整数bh_active和bh_mask，每个无符号整数中的一位对应着bh_base[]中的一个元素
*/
//bh_active相当于一个标记当前系统中产生的”中断“标记”寄存器“
unsigned long bh_active = 0;
//bh_mask是bh_active标记”寄存器“的屏蔽”寄存器“
unsigned long bh_mask = 0;
/*
	在Linux内核中，bottom half通常用"bh"表示，最初用于在特权级较低的上下文中
	完成中断服务的非关键耗时动作，现在也用于一切可在低优先级的上下文中执行的
	异步动作。最早的bottom half实现是借用中断向量表的方式：系统如此定义了一个
	函数指针数组，共有32个函数指针，采用数组索引来访问
*/
struct bh_struct bh_base[32];

/*
	在2.4以前的内核中，每次执行完do_IRQ()中的中断服务例程以后，以及每次系统调
	用结束之前，就在一个叫do_bottom_half()的函数中执行相应的bh函数。
*/
asmlinkage void do_bottom_half(void)
{
	unsigned long active;
	unsigned long mask, left;
	struct bh_struct *bh;

	bh = bh_base;
	//屏蔽掉该屏蔽的”软中断“
	active = bh_active & bh_mask;
	//执行活动的bh
	for (mask = 1, left = ~0 ; left & active ; bh++,mask += mask,left += left) {
		if (mask & active) {
			void (*fn)(void *);
			bh_active &= ~mask;
			fn = bh->routine;
			if (!fn)
				goto bad_bh;
			fn(bh->data);
		}
	}
	return;
bad_bh:
	printk ("irq.c:bad bottom half entry %08lx\n", mask);
}
