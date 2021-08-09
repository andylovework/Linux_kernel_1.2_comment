/*
 *	linux/kernel/resource.c
 *
 * Copyright (C) 1995	Linus Torvalds
 *			David Hinds
 *
 * Kernel io-region resource management
 */
 
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/ioport.h>

/*
	几乎每一种外设都是通过读写设备上的寄存器来进行的。外设寄存器也称为“I/O端口”，通常
	包括控制寄存器、状态寄存器和数据寄存器三大类，而且一个外设的寄存器通常被连续地编址。

	Linux将基于I/O映射方式的或内存映射方式的I/O端口通称为“I/O区域”（I/O region）。
*/

#define IOTABLE_SIZE 32

//Linux设计了一个通用的数据结构resource来描述各种I/O资源
//（如：I/O端口、外设内存、DMA和IRQ等）
typedef struct resource_entry_t {
	u_long from, num;
	const char *name;	//指向此资源的名称
	struct resource_entry_t *next;
} resource_entry_t;

static resource_entry_t iolist = { 0, 0, "", NULL };

static resource_entry_t iotable[IOTABLE_SIZE];

/*
 * This generates the report for /proc/ioports
 */
int get_ioport_list(char *buf)
{
	resource_entry_t *p;
	int len = 0;

	for (p = iolist.next; (p) && (len < 4000); p = p->next)
		len += sprintf(buf+len, "%04lx-%04lx : %s\n",
			   p->from, p->from+p->num-1, p->name);
	if (p)
		len += sprintf(buf+len, "4K limit reached!\n");
	return len;
}

/*
 * The workhorse function: find where to put a new entry
 */
static resource_entry_t *find_gap(resource_entry_t *root,
				  u_long from, u_long num)
{
	unsigned long flags;
	resource_entry_t *p;
	
	if (from > from+num-1)
		return NULL;
	save_flags(flags);
	cli();
	//用一个for循环，两个if判断语句，巧妙的判断了from--from+num+1的区间
	//是否与已有区间有交叉重叠，并且巧妙的实现了排序。
	for (p = root; ; p = p->next) {
		if ((p != root) && (p->from+p->num-1 >= from)) {
			p = NULL;
			break;
		}
		if ((p->next == NULL) || (p->next->from > from+num-1))
			break;
	}
	restore_flags(flags);
	return p;
}

/*
 * Call this from the device driver to register the ioport region.
 */
//在驱动还没独占设备之前，不应对端口进行操作。内核提供了一个注册接口，以允许驱动声明其需要的端口
/*
	request_region告诉内核：要使用first开始的n个端口。参数name为设备名。如果分配成功返回值是非NULL；
	否则无法使用需要的端口(/proc/ioports包含了系统当前所有端口的分配信息，若request_region分配失败时，
	可以查看该文件，看谁先用了你要的端口) 
	
	如果这段I/O端口没有被占用，在我们的驱动程序中就可以使用它。在使用之前，必须向系统登记，以防止被其
	他程序占用。登记后，在/proc/ioports文件中可以看到你登记的io口。

	参数1：io端口的基地址。
	参数2：io端口占用的范围。
	参数3：使用这段io地址的设备名。
	在对I/O口登记后，就可以放心地用inb()， outb()之类的函来访问了。 
*/
void request_region(unsigned int from, unsigned int num, const char *name)
{
	resource_entry_t *p;
	int i;

	for (i = 0; i < IOTABLE_SIZE; i++)
		if (iotable[i].num == 0)
			break;
	if (i == IOTABLE_SIZE)
		printk("warning: ioport table is full\n");
	else {
		//查看所申请的端口中是否有已经被占用的
		p = find_gap(&iolist, from, num);
		if (p == NULL)
			return;
		//这里可以避免竞争条件吗？假设在赋值num时，其他中断中断中断了当前中断，
		//而另一个中断程序也要request_region?
		iotable[i].name = name;
		iotable[i].from = from;
		iotable[i].num = num;
		iotable[i].next = p->next;
		p->next = &iotable[i];
		return;
	}
}

/*
 * This is for compatibility with older drivers.
 * It can be removed when all drivers call the new function.
 */
void snarf_region(unsigned int from, unsigned int num)
{
	request_region(from,num,"No name given.");
}

/* 
 * Call this when the device driver is unloaded
 */
//用完I/O端口后(可能在模块卸载时)，应当调用release_region将I/O端口返还给系统
void release_region(unsigned int from, unsigned int num)
{
	resource_entry_t *p, *q;

	for (p = &iolist; ; p = q) {
		q = p->next;
		if (q == NULL)
			break;
		if ((q->from == from) && (q->num == num)) {
			q->num = 0;
			p->next = q->next;
			return;
		}
	}
}

/*
 * Call this to check the ioport region before probing
 */
/*
	check_region用于检查一个给定的I/O端口集是否可用。如果给定的端口不可用，
	check_region返回一个错误码。不推荐使用该函数，因为即便它返回0（端口可用），
	它也不能保证后面的端口分配操作会成功，因为检查和后面的端口分配并不是一个原
	子操作。而request_region通过加锁来保证操作的原子性，因此是安全的
*/
int check_region(unsigned int from, unsigned int num)
{
	return (find_gap(&iolist, from, num) == NULL) ? -EBUSY : 0;
}

/* Called from init/main.c to reserve(保存) IO ports. */
void reserve_setup(char *str, int *ints)
{
	int i;

	for (i = 1; i < ints[0]; i += 2)
		request_region(ints[i], ints[i+1], "reserved");
}
