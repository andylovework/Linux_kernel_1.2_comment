/*
 *  linux/kernel/exit.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */
#define DEBUG_PROC_TREE

#include <linux/wait.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/resource.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/malloc.h>

#include <asm/segment.h>
extern void sem_exit (void);

int getrusage(struct task_struct *, int, struct rusage *);

static int generate(unsigned long sig, struct task_struct * p)
{
	unsigned long mask = 1 << (sig-1);
	//sa指向进程信号数组中相应的信号“处理器”结构
	struct sigaction * sa = sig + p->sigaction - 1;

	/* always generate signals for traced processes ??? */
	if (p->flags & PF_PTRACED) {
		p->signal |= mask;
		return 1;
	}
	/* don't bother with ignored signals (but SIGCHLD is special) */
	if (sa->sa_handler == SIG_IGN && sig != SIGCHLD)
		return 0;
	/* some signals are ignored by default.. (but SIGCONT already did its deed) */
	if ((sa->sa_handler == SIG_DFL) &&
	    (sig == SIGCONT || sig == SIGCHLD || sig == SIGWINCH))
		return 0;
	p->signal |= mask;
	return 1;
}

//给指定的进程p发送信号指定的信号sig，priv是强制发送信号的标识
//priv:0表示不强制发送信号，非0表示强制发送信号
//理解本函数的关键在于理解相关信号所代表的意义 参见signal.c
int send_sig(unsigned long sig,struct task_struct * p,int priv)
{
	if (!p || sig > 32)
		return -EINVAL;
	//如果priv为零，表示强制发送信号，所以需要验证权限
	//如果不具有超级用户权限，又不是当前session里面的进程
	if (!priv && ((sig != SIGCONT) || (current->session != p->session)) &&
	    (current->euid != p->euid) && (current->uid != p->uid) && !suser())
		return -EPERM;
	if (!sig)
		return 0;
	/*
	 * Forget it if the process is already zombie'd.
	 */
	//不对僵死进程发送信号？
	if (p->state == TASK_ZOMBIE)
		return 0;
	//如果要发送的信号是SIGKILL或者SIGCONT
	if ((sig == SIGKILL) || (sig == SIGCONT)) {
		//如果当前进程处于stop状态，则将其置于TASK_RUNNING状态
		if (p->state == TASK_STOPPED)
			p->state = TASK_RUNNING;
		p->exit_code = 0;
		//消除SIGSTOP SIGTSTP SIGTTIN SIGTTOU
		p->signal &= ~( (1<<(SIGSTOP-1)) | (1<<(SIGTSTP-1)) |
				(1<<(SIGTTIN-1)) | (1<<(SIGTTOU-1)) );
	}
	/* Depends on order SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU */
	//如果信号含有 SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU其中任何信号
	//那么就消除SIGCONT信号
	if ((sig >= SIGSTOP) && (sig <= SIGTTOU)) 
		p->signal &= ~(1<<(SIGCONT-1));
	/* Actually generate the signal */
	generate(sig,p);
	return 0;
}

//在进程退出前调用notify_parent(),给父进程send_sig()后
//将调用wake_up_interruptible()，使信号能够得到及时的响应
void notify_parent(struct task_struct * tsk)
{
	//如果此进程的父进程是任务1
	if (tsk->p_pptr == task[1])
		//将进程的退出信号设为SIGCHLD
		tsk->exit_signal = SIGCHLD;
	//向此进程的父进程发送退出信号
	send_sig(tsk->exit_signal, tsk->p_pptr, 1);
	//当某一进程要结束时，它可以通过调用notify_parent()函数
	//通知父进程以唤醒睡眠在wait_chldexit上的父进程 参见sys_wait4()函数
	wake_up_interruptible(&tsk->p_pptr->wait_chldexit);
}

//release() 主要根据指定进程的任务数据结构指针
//在任务数组中删除指定的进程指针释放相关内存页
void release(struct task_struct * p)
{
	int i;

	if (!p)
		return;
	if (p == current) {
		printk("task releasing itself\n");
		return;
	}
	for (i=1 ; i<NR_TASKS ; i++)
		if (task[i] == p) {
			task[i] = NULL;
			REMOVE_LINKS(p);
			if (STACK_MAGIC != *(unsigned long *)p->kernel_stack_page)
				printk(KERN_ALERT "release: %s kernel stack corruption. Aiee\n", p->comm);
			free_page(p->kernel_stack_page);
			free_page((long) p);
			return;
		}
	panic("trying to release non-existent task");
}

#ifdef DEBUG_PROC_TREE
/*
 * Check to see if a task_struct pointer is present in the task[] array
 * Return 0 if found, and 1 if not found.
 */
int bad_task_ptr(struct task_struct *p)
{
	int i;

	if (!p)
		return 0;
	for (i=0 ; i<NR_TASKS ; i++)
		if (task[i] == p)
			return 0;
	return 1;
}
	
/*
 * This routine scans the pid tree and makes sure the rep invariant still
 * holds.  Used for debugging only, since it's very slow....
 *
 * It looks a lot scarier than it really is.... we're doing nothing more
 * than verifying the doubly-linked list found in p_ysptr and p_osptr, 
 * and checking it corresponds with the process tree defined by p_cptr and 
 * p_pptr;
 */
void audit_ptree(void)
{
	int	i;

	for (i=1 ; i<NR_TASKS ; i++) {
		if (!task[i])
			continue;
		if (bad_task_ptr(task[i]->p_pptr))
			printk("Warning, pid %d's parent link is bad\n",
				task[i]->pid);
		if (bad_task_ptr(task[i]->p_cptr))
			printk("Warning, pid %d's child link is bad\n",
				task[i]->pid);
		if (bad_task_ptr(task[i]->p_ysptr))
			printk("Warning, pid %d's ys link is bad\n",
				task[i]->pid);
		if (bad_task_ptr(task[i]->p_osptr))
			printk("Warning, pid %d's os link is bad\n",
				task[i]->pid);
		if (task[i]->p_pptr == task[i])
			printk("Warning, pid %d parent link points to self\n",
				task[i]->pid);
		if (task[i]->p_cptr == task[i])
			printk("Warning, pid %d child link points to self\n",
				task[i]->pid);
		if (task[i]->p_ysptr == task[i])
			printk("Warning, pid %d ys link points to self\n",
				task[i]->pid);
		if (task[i]->p_osptr == task[i])
			printk("Warning, pid %d os link points to self\n",
				task[i]->pid);
		if (task[i]->p_osptr) {
			if (task[i]->p_pptr != task[i]->p_osptr->p_pptr)
				printk(
			"Warning, pid %d older sibling %d parent is %d\n",
				task[i]->pid, task[i]->p_osptr->pid,
				task[i]->p_osptr->p_pptr->pid);
			if (task[i]->p_osptr->p_ysptr != task[i])
				printk(
		"Warning, pid %d older sibling %d has mismatched ys link\n",
				task[i]->pid, task[i]->p_osptr->pid);
		}
		if (task[i]->p_ysptr) {
			if (task[i]->p_pptr != task[i]->p_ysptr->p_pptr)
				printk(
			"Warning, pid %d younger sibling %d parent is %d\n",
				task[i]->pid, task[i]->p_osptr->pid,
				task[i]->p_osptr->p_pptr->pid);
			if (task[i]->p_ysptr->p_osptr != task[i])
				printk(
		"Warning, pid %d younger sibling %d has mismatched os link\n",
				task[i]->pid, task[i]->p_ysptr->pid);
		}
		if (task[i]->p_cptr) {
			if (task[i]->p_cptr->p_pptr != task[i])
				printk(
			"Warning, pid %d youngest child %d has mismatched parent link\n",
				task[i]->pid, task[i]->p_cptr->pid);
			if (task[i]->p_cptr->p_ysptr)
				printk(
			"Warning, pid %d youngest child %d has non-NULL ys link\n",
				task[i]->pid, task[i]->p_cptr->pid);
		}
	}
}
#endif /* DEBUG_PROC_TREE */

/*
 * This checks not only the pgrp, but falls back on the pid if no
 * satisfactory pgrp is found. I dunno - gdb doesn't work correctly
 * without this...
 */
 //获取process group的session ID
int session_of_pgrp(int pgrp)
{
	struct task_struct *p;
	int fallback;

	fallback = -1;
	for_each_task(p) {
 		if (p->session <= 0)
 			continue;
		if (p->pgrp == pgrp)
			return p->session;
		if (p->pid == pgrp)
			fallback = p->session;
	}
	return fallback;
}

/*
 * kill_pg() sends a signal to a process group: this is what the tty
 * control characters do (^C, ^Z etc)
 */
 //向一个进程组发送一个信号
int kill_pg(int pgrp, int sig, int priv)
{
	struct task_struct *p;
	int err,retval = -ESRCH;
	int found = 0;

	if (sig<0 || sig>32 || pgrp<=0)
		return -EINVAL;
	//遍历进程数组
	for_each_task(p) {
		//如果当前被遍历的进程的组id等于pgrp
		if (p->pgrp == pgrp) {
			//向此进程发送信号sig
			if ((err = send_sig(sig,p,priv)) != 0)
				//发送失败
				retval = err;
			else
				//发送成功
				found++;
		}
	}
	return(found ? 0 : retval);
}

/*
 * kill_sl() sends a signal to the session leader: this is used
 * to send SIGHUP to the controlling process of a terminal when
 * the connection is lost.
 */
//向属于会话的所有进程发送信号
int kill_sl(int sess, int sig, int priv)
{
	struct task_struct *p;
	int err,retval = -ESRCH;
	int found = 0;

	if (sig<0 || sig>32 || sess<=0)
		return -EINVAL;
	for_each_task(p) {
		if (p->session == sess && p->leader) {
			if ((err = send_sig(sig,p,priv)) != 0)
				retval = err;
			else
				found++;
		}
	}
	return(found ? 0 : retval);
}

//向指定的进程发送信号
int kill_proc(int pid, int sig, int priv)
{
 	struct task_struct *p;

	if (sig<0 || sig>32)
		return -EINVAL;
	for_each_task(p) {
		if (p && p->pid == pid)
			return send_sig(sig,p,priv);
	}
	return(-ESRCH);
}

/*
 * POSIX specifies that kill(-1,sig) is unspecified, but what we have
 * is probably wrong.  Should make it like BSD or SYSV.
 */
/*
	应用程序发送信号时，主要通过kill进行。注意：不要被“kill”迷惑，
	它并不是发送SIGKILL信号专用函数。这个函数主要通过系统调用sys_kill
	进入内核，它接收两个参数：
　　第一个参数为目标进程id，kill()可以向进程（或进程组），线程（轻权线程）
	发送信号，因此pid有以下几种情况：
　　● pid>0：目标进程（可能是轻权进程）由pid指定。
　　● pid=0：信号被发送到当前进程组中的每一个进程。
　　● pid=-1：信号被发送到任何一个进程，init进程（PID=1）和以及当前进程无法发送信号的进程除外。
　　● pid<-1：信号被发送到目标进程组，其id由参数中的pid的绝对值指定。
　　第二个参数为需要发送的信号。
*/
asmlinkage int sys_kill(int pid,int sig)
{
	int err, retval = 0, count = 0;

	//pid=0：信号被发送到当前进程组中的每一个进程
	if (!pid)
		return(kill_pg(current->pgrp,sig,0));
	//pid=-1：信号被发送到任何一个进程，init进程（PID=1）和以及当前进程无法发送信号的进程除外
	if (pid == -1) {
		struct task_struct * p;
		for_each_task(p) {
			if (p->pid > 1 && p != current) {
				++count;
				if ((err = send_sig(sig,p,0)) != -EPERM)
					retval = err;
			}
		}
		return(count ? retval : -ESRCH);
	}
	//pid<-1：信号被发送到目标进程组，其id由参数中的pid的绝对值指定
	if (pid < 0) 
		return(kill_pg(-pid,sig,0));
	/* Normal kill */
	//pid>0：目标进程（可能是轻权进程）由pid指定
	return(kill_proc(pid,sig,0));
}

/*
 * Determine if a process group is "orphaned"(孤儿), according to the POSIX
 * definition in 2.2.2.52.  Orphaned process groups are not to be affected
 * by terminal-generated stop signals.  Newly orphaned process groups are 
 * to receive a SIGHUP and a SIGCONT.
 * 
 * "I ask you, have you ever known what it is to be an orphan?"
 */
 //判断一个进程组是不是孤儿进程组
/*
		孤儿进程组的由来：
	当一个终端控制进程（即会话首进程）终止后，那么这个终端可以
	用来建立一个新的会话。这可能会产生一个问题，原来旧的会话
	（一个或者多个进程组的集合）中的任一进程再次访问这个的终端。
	为了防止这类问题的产生，于是就有了孤儿进程组的概念。当一个
	进程组成为孤儿进程组时，posix.1要求向孤儿进程组中处于停止
	状态的进程发送SIGHUP（挂起）信号，系统对于这种信号的默认处
	理是终止进程，然而如果无视这个信号或者另行处理的话那么这个
	挂起进程仍可以继续执行
*/
/*
    	POSIX.1对孤儿进程组的定义：
	该组中每个成员的父进程要么是该组的一个成员，
	要么不是该组所属会话的成员。或者用另外一种表示
	一个进程组不是孤儿进程组的条件是：该组中有一个
	进程，其父进程在属于同一个会话的另一个组中(因此
	可以认为此进程组还处于创造它的session的控制中)
	
	所谓的孤儿进程组简单点说就是脱离了创造它的
	session控制的，离开其session眼线的进程组
*/
int is_orphaned_pgrp(int pgrp)
{
	struct task_struct *p;

	for_each_task(p) {
		//如果p不属于pgrp进程组，或者p属于pgrp进程组，但是其是僵尸进程
		//则continue
		if ((p->pgrp != pgrp) || 
		    (p->state == TASK_ZOMBIE) ||
		    (p->p_pptr->pid == 1))
			continue;
		//执行到这里，说明p属于进程组pgrp，但是p不是僵死进程
		//所以要判断其父进程是否在属于同一个会话的另一个组中
		//如果是的话，说明此进程组不是孤儿进程组，则返回0
		if ((p->p_pptr->pgrp != pgrp) &&
		    (p->p_pptr->session == p->session))
			return 0;
	}
	return(1);	/* (sighing) "Often!" */
}

//判断指定的进程组中是否含有状态为TASK_STOPPED的进程
static int has_stopped_jobs(int pgrp)
{
	struct task_struct * p;

	for_each_task(p) {
		if (p->pgrp != pgrp)
			continue;
		if (p->state == TASK_STOPPED)
			return(1);
	}
	return(0);
}

//将父进程为father的进程的原始父进程改为进程0或1
static void forget_original_parent(struct task_struct * father)
{
	struct task_struct * p;

	for_each_task(p) {
		if (p->p_opptr == father)
			if (task[1])
				p->p_opptr = task[1];
			else
				p->p_opptr = task[0];
	}
}

static void exit_files(void)
{
	int i;

	for (i=0 ; i<NR_OPEN ; i++)
		if (current->files->fd[i])
			sys_close(i);
}

static void exit_fs(void)
{
	iput(current->fs->pwd);
	current->fs->pwd = NULL;
	iput(current->fs->root);
	current->fs->root = NULL;
}

//《Linux内核源代码情景分析》中对此函数的讲解分析的比较深入，特别是对
//内核为什么会保留进程的一些数据结构内存空间，也就是僵死进程的原因的分析
//内核需要保留进程的内核堆栈，以应对随时而来的中断。当exit调度到别的程序中
//去时，才会由其父进程销毁僵死进程的内核数据结构空间
/*
	永不返回的函数（never return function）
    了解C语言的人都知道一个函数的最后一个语句通常是return语句。编译器在处理返回语句时，
	除了将返回值保存起来之外，最重要的任务就是清理堆栈。具体来说，就是将参数以及局部变
	量从堆栈中弹出。然后再从堆栈中得到调用函数时的PC寄存器的值，并将其加一个指令的长度，
	从而得到下一条指令的地址。再将这个地址放入PC寄存器中，完成整个返回流程，接着程序就
	会继续执行下去了。
    对于返回值是void类型，也就是无返回值的函数，保存返回值是没有意义的，但它仍然会执行
	清理堆栈的操作。
    以上提到的这些，基本上适用于99.99%的场合。但凡事无绝对，在一些特殊的地方，例如操作
	系统内核中的某些函数，就不见得符合上边所说的这些。永不返回的函数就是其中之一。
    在Linux源代码中，一个永不返回的函数通常拥有一个类似如下函数的声明：
    NORET_TYPE void do_exit(long code)
    考虑到NORET_TYPE的定义：
    #define NORET_TYPE    /**/
/*  因此，NORET_TYPE在这里仅仅起到方便阅读代码的作用，而并没有什么其他的特殊作用。
    看到do_exit函数，可能熟悉Linux内核的朋友已经猜出永不返回的函数和普通函数有什么区别了。
	没错，do_exit函数是销毁进程的最后一步。由于进程已经销毁，从进程堆栈中获得下一条指令的
	地址就显得没有什么意义了。do_exit函数会调用schedule函数进行进程切换，从另一个进程的堆
	栈中获得相关寄存器的值，并恢复那个进程的执行。因此do_exit函数在正常情况下是不会返回的，
	一个调用了do_exit函数的函数，其位于do_exit函数之后的语句是不会执行到的。因此那个函数也
	成为了永不返回的函数。
*/
NORET_TYPE void do_exit(long code)
{
	struct task_struct *p;

	if (intr_count) {
		printk("Aiee, killing interrupt handler\n");
		intr_count = 0;
	}
fake_volatile:
	//设置当前进程的退出标志
	current->flags |= PF_EXITING;
	//对当前进程的信号量集合做退出处理
	sem_exit();
	/* Release all mmaps. */
	exit_mmap(current);
	//frees up all page tables of a process 
	free_page_tables(current);
	exit_files();
	exit_fs();
	exit_thread();
	/*这里好像没有对进程的内核定时器做处理*/
	forget_original_parent(current);
	/* 
	 * Check to see if any process groups have become orphaned
	 * as a result of our exiting, and if they have any stopped
	 * jobs, send them a SIGUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 *
	 * Case i: Our father is in a different pgrp than we are
	 * and we were the only connection outside, so our pgrp
	 * is about to become orphaned.
 	 */
	//前两个判断条件的意义在于判断当前进程的父进程是否
	//在属于同一个会话的另一个组中,若是，则当前进程所在的进程
	//组可能会由于此进程的退出而成为孤儿进程组。所以第三个判断条件
	//意在判断此进程组是否成为了孤儿进程组，若是，则再继续判断
	//此孤儿进程组中是否存在暂停状态的进程
	if ((current->p_pptr->pgrp != current->pgrp) &&
	    (current->p_pptr->session == current->session) &&
	    is_orphaned_pgrp(current->pgrp) &&
	    has_stopped_jobs(current->pgrp)) {
		kill_pg(current->pgrp,SIGHUP,1);
		kill_pg(current->pgrp,SIGCONT,1);
	}
	/* Let father know we died */
	notify_parent(current);
	
	/*
	 * This loop does two things:
	 * 
  	 * A.  Make init inherit all the child processes
	 * B.  Check to see if any process groups have become orphaned
	 *	as a result of our exiting, and if they have any stopped
	 *	jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 */
	//此循环做两件事：第一件事是重新调整进程树，第二件事是判断是否出现孤儿进程组
	//current->p_cptr指向当前进程最后一次创建的子进程，通过它可以遍历进程的所有子进程
	//调整进程树，就是将当前进程的所有（直接）子进程的父进程改为进程0或1，并链入到进程0或1
	//的直接子进程链表中去
	while ((p = current->p_cptr) != NULL) {
		//要想明白下面的代码，首先要明白进程间的组织关系。可参见《深入理解Linux内核》
		//第三章中的内容（主要是一张“五个进程间的亲属关系”图解）
		current->p_cptr = p->p_osptr;
		p->p_ysptr = NULL;
		p->flags &= ~(PF_PTRACED|PF_TRACESYS);
		//将p的父进程改为进程0或1
		if (task[1] && task[1] != current)
			p->p_pptr = task[1];
		else
			p->p_pptr = task[0];
		//下面的三行代码的意义是将当前进程的一个子进程p链入到
		//进程0或1的子进程链表中去
		p->p_osptr = p->p_pptr->p_cptr;
		p->p_osptr->p_ysptr = p;
		p->p_pptr->p_cptr = p;
		if (p->state == TASK_ZOMBIE)
			notify_parent(p);
		/*
		 * process group orphan check
		 * Case ii: Our child is in a different pgrp 
		 * than we are, and it was the only connection
		 * outside, so the child pgrp is now orphaned.
		 */
		if ((p->pgrp != current->pgrp) &&
		    (p->session == current->session) &&
		    is_orphaned_pgrp(p->pgrp) &&
		    has_stopped_jobs(p->pgrp)) {
			kill_pg(p->pgrp,SIGHUP,1);
			kill_pg(p->pgrp,SIGCONT,1);
		}
	}
	if (current->leader)
		//脱离当前的tty 并向进程显示终端的组发送SIGHUP 和SIGCONT
		disassociate_ctty(1);
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	current->state = TASK_ZOMBIE;
	current->exit_code = code;
	current->mm->rss = 0;
#ifdef DEBUG_PROC_TREE
	audit_ptree();
#endif
	if (current->exec_domain && current->exec_domain->use_count)
		(*current->exec_domain->use_count)--;
	if (current->binfmt && current->binfmt->use_count)
		(*current->binfmt->use_count)--;
	schedule();
/*
 * In order to get rid of the "volatile function does return" message
 * I did this little loop that confuses gcc to think do_exit really
 * is volatile. In fact it's schedule() that is volatile in some
 * circumstances: when current->state = ZOMBIE, schedule() never
 * returns.
 *
 * In fact the natural way to do all this is to have the label and the
 * goto right after each other, but I put the fake_volatile label at
 * the start of the function just in case something /really/ bad
 * happens, and the schedule returns. This way we can try again. I'm
 * not paranoid: it's just that everybody is out to get me.
 */
	goto fake_volatile;
}

asmlinkage int sys_exit(int error_code)
{
	do_exit((error_code&0xff)<<8);
}

/*
	wait4会挂起当前进程，等待指定的子进程状态改变，并且会返回关于子进程使用资源的信息。
	所谓的状态的改变包括终止，挂起所有的进程状态的改变。
	其次，在linux中进程终止后不会释放其资源，它会进入一种僵死的状态，内核会为僵死态的
	进程保留最少的信息(进程标识，终止状态和资源使用信息).而wait4就可以释放与子进程关联
	的资源。如果父进程不执行wait,而父进程又死亡后，僵死态的子进程会被指定成系统初始化
	进程init的子进程.

	pid是要关注的子进程的pid(进程标识号)
	status子进程的返回状态。
	options进程等待选项
	rusage死亡进程资源使用记录
	
	pid这个很好理解，进程标识号，也是你ps -le显示出来的pid。如果指定了pid则会在指定pid
	发生变化后唤醒父进程，此外还有一下4种情况:
	< -1   which means to wait for any child process whose process group ID is equal to the absolute value of pid.
	-1     which means to wait for any child process; this is equivalent to calling wait3.
	0      which means to wait for any child process whose process group ID is equal to that of the calling process.
	> 0    which means to wait for the child whose process ID is equal to the value of pid.
*/
//进程关闭后会给父进程一个信号，而父进程会在内核入口wait4（）等待信号。
asmlinkage int sys_wait4(pid_t pid,unsigned long * stat_addr, int options, struct rusage * ru)
{
	int flag, retval;
	struct wait_queue wait = { current, NULL };
	struct task_struct *p;

	if (stat_addr) {
		flag = verify_area(VERIFY_WRITE, stat_addr, 4);
		if (flag)
			return flag;
	}
	//将数据结构wait加入到wait_chldexit队列中
	//唤醒操作参见notify_parent()函数
	add_wait_queue(&current->wait_chldexit,&wait);
repeat:
	flag=0;
	//遍历当前进程的所有直接子进程 找到要wait的子进程 并进行处理
 	for (p = current->p_cptr ; p ; p = p->p_osptr) {
		if (pid>0) {
			if (p->pid != pid)
				continue;
		//0：which means to wait for any child process whose process group ID is equal to that of the calling process.
		} else if (!pid) {
			if (p->pgrp != current->pgrp)
				continue;
		//< -1：which means to wait for any child process whose process group ID is equal to the absolute value of pid
		} else if (pid != -1) {
			if (p->pgrp != -pid)
				continue;
		}
		/* wait for cloned processes iff the __WCLONE flag is set */
		if ((p->exit_signal != SIGCHLD) ^ ((options & __WCLONE) != 0))
			continue;
		flag = 1;
		switch (p->state) {
			case TASK_STOPPED:
				if (!p->exit_code)
					continue;
				if (!(options & WUNTRACED) && !(p->flags & PF_PTRACED))
					continue;
				if (stat_addr)
					put_fs_long((p->exit_code << 8) | 0x7f,
						stat_addr);
				p->exit_code = 0;
				if (ru != NULL)
					getrusage(p, RUSAGE_BOTH, ru);
				retval = p->pid;
				goto end_wait4;
			case TASK_ZOMBIE:
				//cutime:child user time 
				//cstime:child supervisor tmie 
				current->cutime += p->utime + p->cutime;
				current->cstime += p->stime + p->cstime;
				current->mm->cmin_flt += p->mm->min_flt + p->mm->cmin_flt;
				current->mm->cmaj_flt += p->mm->maj_flt + p->mm->cmaj_flt;
				if (ru != NULL)
					getrusage(p, RUSAGE_BOTH, ru);
				flag = p->pid;
				if (stat_addr)
					put_fs_long(p->exit_code, stat_addr);
				if (p->p_opptr != p->p_pptr) {
					REMOVE_LINKS(p);
					p->p_pptr = p->p_opptr;
					SET_LINKS(p);
					notify_parent(p);
				} else
					release(p);
#ifdef DEBUG_PROC_TREE
				audit_ptree();
#endif
				retval = flag;
				goto end_wait4;
			default:
				continue;
		}
	}
	if (flag) {
		retval = 0;
		if (options & WNOHANG)
			goto end_wait4;
		current->state=TASK_INTERRUPTIBLE;
		schedule();
		current->signal &= ~(1<<(SIGCHLD-1));
		retval = -ERESTARTSYS;
		if (current->signal & ~current->blocked)
			goto end_wait4;
		goto repeat;
	}
	retval = -ECHILD;
end_wait4:
	remove_wait_queue(&current->wait_chldexit,&wait);
	return retval;
}

/*
 * sys_waitpid() remains for compatibility. waitpid() should be
 * implemented by calling sys_wait4() from libc.a.
 */
 //wait4其实和waitpid一样，只是参数是4个，可以跟踪子进程的cpu时间片等信息
asmlinkage int sys_waitpid(pid_t pid,unsigned long * stat_addr, int options)
{
	return sys_wait4(pid, stat_addr, options, NULL);
}
