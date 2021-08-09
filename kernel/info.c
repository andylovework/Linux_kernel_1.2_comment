/*
 * linux/kernel/info.c
 *
 * Copyright (C) 1992 Darren Senn
 */

/* This implements the sysinfo() system call */

#include <asm/segment.h>

#include <linux/sched.h>
#include <linux/string.h>
#include <linux/unistd.h>
#include <linux/types.h>
#include <linux/mm.h>
/*
	struct sysinfo结构定义于linux/include/linux/kernel.h
*/
//获取系统总体统计信息
asmlinkage int sys_sysinfo(struct sysinfo *info)
{
	int error;
	struct sysinfo val;
	struct task_struct **p;

	error = verify_area(VERIFY_WRITE, info, sizeof(struct sysinfo));
	if (error)
		return error;
	memset((char *)&val, 0, sizeof(struct sysinfo));

	val.uptime = jiffies / HZ;

	//获取系统负载信息
	val.loads[0] = avenrun[0] << (SI_LOAD_SHIFT - FSHIFT);
	val.loads[1] = avenrun[1] << (SI_LOAD_SHIFT - FSHIFT);
	val.loads[2] = avenrun[2] << (SI_LOAD_SHIFT - FSHIFT);

	//统计当前进程数
	for (p = &LAST_TASK; p > &FIRST_TASK; p--)
		if (*p) val.procs++;

	//统计当前系统内存和交换页面信息
	si_meminfo(&val);
	si_swapinfo(&val);

	memcpy_tofs(info, &val, sizeof(struct sysinfo));
	return 0;
}
