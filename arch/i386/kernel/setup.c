/*
 *  linux/arch/i386/kernel/setup.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 */
/*
 * This file handles the architecture-dependent parts of process handling..
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/stddef.h>
#include <linux/unistd.h>
#include <linux/ptrace.h>
#include <linux/malloc.h>
#include <linux/ldt.h>
#include <linux/user.h>
#include <linux/a.out.h>
#include <linux/tty.h>
#include <linux/ioport.h>

#include <asm/segment.h>
#include <asm/system.h>

/*
 * Tell us the machine setup..
 */
char hard_math = 0;		/* set by boot/head.S */
char x86 = 0;			/* set by boot/head.S to 3 or 4 */
char x86_model = 0;		/* set by boot/head.S */
char x86_mask = 0;		/* set by boot/head.S */
int x86_capability = 0;		/* set by boot/head.S */
int fdiv_bug = 0;		/* set if Pentium(TM) with FP bug */

char x86_vendor_id[13] = "Unknown";

char ignore_irq13 = 0;		/* set if exception 16 works */
char wp_works_ok = 0;		/* set if paging hardware honours WP */ 
char hlt_works_ok = 1;		/* set if the "hlt" instruction works */

/*
 * Bus types ..
 */
int EISA_bus = 0;

/*
 * Setup options
 */
struct drive_info_struct { char dummy[32]; } drive_info;
struct screen_info screen_info;

unsigned char aux_device_present;
extern int ramdisk_size;
extern int root_mountflags;
extern int etext, edata, end;

/*
	empty_zero_page页(即零页)，零页存放的是系统启动参数和命令行参数
	empty_zero_page中存放的是在操作系统的引导过程中所收集的一些数据，
	叫做引导参数。因为这个页面开始的内容全为0，所以叫做“零页”，代码中
	常常通过宏定义ZERO_PAGE来引用这个页面。不过，这个页面要到初始化完
	成，系统转入正常运行时才会用到

	empty_zero_page：该页的前2KB空间用来存储setup.s保存在内存参数区的
	来自BIOS的系统硬件参数；后2KB空间作为命令行缓冲区
*/
extern char empty_zero_page[PAGE_SIZE];

/*
 * This is set up by the setup-routine at boot-time
 */
//宏PARAM就是empty_zero_page的起始位置
#define PARAM	empty_zero_page
#define EXT_MEM_K (*(unsigned short *) (PARAM+2))  //扩展内存大小
#define DRIVE_INFO (*(struct drive_info_struct *) (PARAM+0x80))  //硬盘参数表
#define SCREEN_INFO (*(struct screen_info *) (PARAM+0))
#define MOUNT_ROOT_RDONLY (*(unsigned short *) (PARAM+0x1F2))
#define RAMDISK_SIZE (*(unsigned short *) (PARAM+0x1F8))
#define ORIG_ROOT_DEV (*(unsigned short *) (PARAM+0x1FC))  //根文件系统的设备号
#define AUX_DEVICE_INFO (*(unsigned char *) (PARAM+0x1FF))
#define COMMAND_LINE ((char *) (PARAM+2048))
#define COMMAND_LINE_SIZE 256 //一个扇区容量

static char command_line[COMMAND_LINE_SIZE] = { 0, };

void setup_arch(char **cmdline_p,
	unsigned long * memory_start_p, unsigned long * memory_end_p)
{
	unsigned long memory_start, memory_end;
	char c = ' ', *to = command_line, *from = COMMAND_LINE;
	int len = 0;

	//获取外设的参数,并写入相应内存单元
 	ROOT_DEV = ORIG_ROOT_DEV;
 	drive_info = DRIVE_INFO;
 	screen_info = SCREEN_INFO;
	aux_device_present = AUX_DEVICE_INFO;
	memory_end = (1<<20) + (EXT_MEM_K<<10);
	memory_end &= PAGE_MASK;
	ramdisk_size = RAMDISK_SIZE;
#ifdef CONFIG_MAX_16M
	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;
#endif
	if (MOUNT_ROOT_RDONLY)
		root_mountflags |= MS_RDONLY;
	memory_start = (unsigned long) &end;
	//设置 init_task.mm 代码结构在内存中的起点和终点以及数据段的终点
	init_task.mm->start_code = TASK_SIZE;
	init_task.mm->end_code = TASK_SIZE + (unsigned long) &etext;
	init_task.mm->end_data = TASK_SIZE + (unsigned long) &edata;
	init_task.mm->brk = TASK_SIZE + (unsigned long) &end;

	//将命令行参数复制到command_line数组中.
	for (;;) {
		//"mem="命令将会重新设置内存大小
		if (c == ' ' && *(unsigned long *)from == *(unsigned long *)"mem=") {
			memory_end = simple_strtoul(from+4, &from, 0);
			if ( *from == 'K' || *from == 'k' ) {
				memory_end = memory_end << 10;
				from++;
			} else if ( *from == 'M' || *from == 'm' ) {
				memory_end = memory_end << 20;
				from++;
			}
		}
		c = *(from++);
		if (!c)
			break;
		if (COMMAND_LINE_SIZE <= ++len)
			break;
		*(to++) = c;
	}
	*to = '\0';
	*cmdline_p = command_line;
	*memory_start_p = memory_start;
	*memory_end_p = memory_end;
	/* request io space for devices used on all i[345]86 PC'S */
	//一下注册的端口号应该是所有i[345]86 PC机上的标准
	request_region(0x00,0x20,"dma1");
	request_region(0x40,0x20,"timer");
	request_region(0x70,0x10,"rtc");
	request_region(0x80,0x20,"dma page reg");
	request_region(0xc0,0x20,"dma2");
	request_region(0xf0,0x2,"npu");
	request_region(0xf8,0x8,"npu");
}
