/*
 * linux/kernel/itimer.c
 *
 * Copyright (C) 1992 Darren Senn
 */
/* These are all the functions necessary to implement itimers */
/*
				Linux系统进程间隔定时器Itimer
	所谓“间隔定时器（Interval Timer，简称itimer）就是指定时器采用“间隔”值（interval）来作为计时方式，
	当定时器启动后，间隔值interval将不断减小。当 interval值减到0时，我们就说该间隔定时器到期。内核
	动态定时器相比，二者最大的区别在于定时器的计时方式不同。内核定时器是通过它的到期时刻
	expires值来计时的，当全局变量jiffies值大于或等于内核动态定时器的expires值时，我们说内核内核定时器到期。
	而间隔定时器则实际上是通过一个不断减小的计数器来计时的。虽然这两种定时器并不相同，但却也是相互联系的。
	假如我们每个时钟节拍都使间隔定时器的间隔计数器减 1，那么在这种情形下间隔定时器实际上就是内核动态定时器
	（下面我们会看到进程的真实间隔定时器就是这样通过内核定时器来实现的）。
　　间隔定时器主要被应用在用户进程上。每个Linux进程都有三个相互关联的间隔定时器。其各自的间隔计数器都定义在
	进程的task_struct结构中，如下所示（include/linux/sched.h）：
	　　struct task_struct｛
		　　……
		　　unsigned long it_real_value, it_prof_value, it_virt_value;
		　　unsigned long it_real_incr, it_prof_incr, it_virt_incr;
		　　struct timer_list real_timer;
		　　……
	　　}
　　A>> 真实间隔定时器（ITIMER_REAL）：这种间隔定时器在启动后，不管进程是否运行，每个时钟滴答都将其间隔计数器减1。
	当减到0值时，内核向进程发送SIGALRM信号。结构类型task_struct中的成员it_real_incr则表示真实间隔定时器的间隔计数器的
	初始值，而成员 it_real_value则表示真实间隔定时器的间隔计数器的当前值。由于这种间隔定时器本质上与上一节的内核定时
	器时一样的，因此Linux实际上是通过real_timer这个内嵌在task_struct结构中的内核动态定时器来实现真实间隔定时器ITIMER_REAL的。
　　
	B>> 虚拟间隔定时器ITIMER_VIRT：也称为进程的用户态间隔定时器。结构类型task_struct中成员it_virt_incr和 it_virt_value分别
	表示虚拟间隔定时器的间隔计数器的初始值和当前值，二者均以时钟滴答次数位计数单位。当虚拟间隔定时器启动后，只有当进程在
	用户态下运行时，一次时钟滴答才能使间隔计数器当前值it_virt_value减1。当减到0值时，内核向进程发送SIGVTALRM信号
	（虚拟闹钟信号），并将it_virt_value重置为初值it_virt_incr。具体请见7.4.3节中的do_it_virt()函数的实现。
　　
	C>> PROF间隔定时器ITIMER_PROF：进程的task_struct结构中的it_prof_value和it_prof_incr成员分别表示PROF间隔定时器的
	间隔计数器的当前值和初始值（均以时钟滴答为单位）。当一个进程的PROF间隔定时器启动后，则只要该进程处于运行中，而不管
	是在用户态或核心态下执行，每个时钟滴答都使间隔计数器it_prof_value值减1。当减到0值时，内核向进程发送SIGPROF信号
	并将 it_prof_value重置为初值it_prof_incr。
*/
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/mm.h>

#include <asm/segment.h>

/*	由于间隔定时器的间隔计数器的内部表示方式与外部表现方式互不相同，
	因此有必要实现以微秒为单位的timeval结构和为时钟滴答次数单位的 jiffies之间的相互转换。
*/

static unsigned long tvtojiffies(struct timeval *value)
{
	return((unsigned long )value->tv_sec * HZ +
		(unsigned long )(value->tv_usec + (1000000 / HZ - 1)) /
		(1000000 / HZ));
}

static void jiffiestotv(unsigned long jiffies, struct timeval *value)
{
	value->tv_usec = (jiffies % HZ) * (1000000 / HZ);
	value->tv_sec = jiffies / HZ;
	return;
}

int _getitimer(int which, struct itimerval *value)
{
	//用局部变量val和interval分别表示待查询间隔定时器的间隔计数器的当前值和初始值
	register unsigned long val, interval;

	switch (which) {
	//如果which＝ITIMER_REAL，则查询当前进程的ITIMER_REAL间隔定时器
	case ITIMER_REAL:
		val = current->it_real_value;
		interval = current->it_real_incr;
		break;
	//如果which＝ITIMER_VIRT，则查询当前进程的ITIMER_VIRT间隔定时器
	case ITIMER_VIRTUAL:
		val = current->it_virt_value;
		interval = current->it_virt_incr;
		break;
	//如果which＝ITIMER_PROF，则查询当前进程的ITIMER_PROF间隔定时器
	case ITIMER_PROF:
		val = current->it_prof_value;
		interval = current->it_prof_incr;
		break;
	default:
		return(-EINVAL);
	}
/*
	通过转换函数jiffiestotv()将val和interval转换成timeval格式的时间值，
	并保存到value->it_value和value->it_interval中，作为查询结果返回
*/
	jiffiestotv(val, &value->it_value);
	jiffiestotv(interval, &value->it_interval);
	return(0);
}

//用于查询调用进程的三个间隔定时器的信息
/*
	函数sys_getitimer()有两个参数：（1）which，指定查询调用进程的哪一个间隔定时器，
	其取值可以是ITIMER_REAL、ITIMER_VIRT和ITIMER_PROF三者之一。（2）value指针，指
	向用户空间中的一个itimerval结构，用于接收查询结果
	显然，sys_getitimer()函数主要通过_getitimer()函数来查询当前进程的间隔定时器
	信息，并将查询结果保存在内核空间的结构变量get_buffer中。然后，调用memcpy_tofs()
	宏将get_buffer中结果拷贝到用户空间缓冲区中。
*/
asmlinkage int sys_getitimer(int which, struct itimerval *value)
{
	int error;
	struct itimerval get_buffer;

	if (!value)
		return -EFAULT;
	error = _getitimer(which, &get_buffer);
	if (error)
		return error;
	error = verify_area(VERIFY_WRITE, value, sizeof(struct itimerval));
	if (error)
		return error;
	memcpy_tofs(value, &get_buffer, sizeof(get_buffer));
	return 0;
}

int _setitimer(int which, struct itimerval *value, struct itimerval *ovalue)
{
	register unsigned long i, j;
	int k;

/*
	首先调用tvtojiffies()函数将timeval格式的初始值和当前值
	转换成以时钟滴答为单位的时间值。并分别保存在局部变量i和j中
*/
	//it_interval成员表示间隔计数器的初始值
	i = tvtojiffies(&value->it_interval);
	//it_value成员表示间隔计数器的当前值
	j = tvtojiffies(&value->it_value);
/*
	如果ovalue指针非空，则调用_getitimer()函数查询指定间隔
	定时器的原来信息。如果_getitimer()函数返回负值，说明出错
	因此就要直接返回错误值。否则继续向下执行开始真正地设置指定
	的间隔定时器
*/
	if (ovalue && (k = _getitimer(which, ovalue)) < 0)
		return k;
	switch (which) {
		//如果which=ITITMER_REAL，表示设置ITIMER_REAL间隔定时器
		case ITIMER_REAL:
			//如果j=0，说明不必启动real_timer定时器.这种间隔定时器在启动后，
			//不管进程是否运行，每个时钟滴答都将其间隔计数器减1。当减到0值时
			//内核向进程发送SIGALRM信号。
			if (j) {
				//j不为0，则需要启动real_timer定时器
				//要明白这里为什么这么做 需要理解itimer_ticks和itimer_next两个全局变量的意义
				//参见sched.c文件中do_timer()函数中对itimer_ticks的注释以及schedule()函数中
				//对itimer_next的注释，便可深刻的理解内核间隔定时器的运行机制
				j += 1+itimer_ticks;
				if (j < itimer_next)
					itimer_next = j;
			}
			current->it_real_value = j;
			current->it_real_incr = i;
			break;
		case ITIMER_VIRTUAL:
			if (j)
				j++;
			current->it_virt_value = j;
			current->it_virt_incr = i;
			break;
		case ITIMER_PROF:
			if (j)
				j++;
			current->it_prof_value = j;
			current->it_prof_incr = i;
			break;
		default:
			return -EINVAL;
	}
	return 0;
}

/*
	函数sys_setitimer()不仅设置调用进程的指定间隔定时器，而且还返回该间隔定时器的原有信息。
	它有三个参数：（1）which，含义与 sys_getitimer()中的参数相同。（2）输入参数value，指向
	用户空间中的一个itimerval结构，含有待设置的新值。（3）输出参数ovalue，指向用户空间中的
	一个itimerval结构，用于接收间隔定时器的原有信息。
*/
asmlinkage int sys_setitimer(int which, struct itimerval *value, struct itimerval *ovalue)
{
	int error;
	struct itimerval set_buffer, get_buffer;

	if (value) {
		error = verify_area(VERIFY_READ, value, sizeof(*value));
		if (error)
			return error;
		memcpy_fromfs(&set_buffer, value, sizeof(set_buffer));
	} else
		memset((char *) &set_buffer, 0, sizeof(set_buffer));

	if (ovalue) {
		error = verify_area(VERIFY_WRITE, ovalue, sizeof(struct itimerval));
		if (error)
			return error;
	}

	error = _setitimer(which, &set_buffer, ovalue ? &get_buffer : 0);
	if (error || !ovalue)
		return error;

	memcpy_tofs(ovalue, &get_buffer, sizeof(get_buffer));
	return error;
}
