/*
 *  linux/kernel/fork.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s).
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
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

#include <asm/segment.h>
#include <asm/system.h>

long last_pid=0;

//找一个空闲的进程任务task[]下标即任务号nr，并取得一个不重复的pid.
static int find_empty_process(void)
{
	int free_task;
	int i, tasks_free;
	int this_user_tasks;

repeat:
	if ((++last_pid) & 0xffff8000)
		last_pid=1;
	this_user_tasks = 0;
	tasks_free = 0;
	free_task = -EAGAIN;
	i = NR_TASKS;
	//将任务0 排除在外
	while (--i > 0) {
		if (!task[i]) {
			free_task = i;
			tasks_free++;	//统计当前系统剩余的空闲进程槽数
			continue;
		}
		if (task[i]->uid == current->uid)
			this_user_tasks++;	//统计当前用户所拥有的进程总数
		//如果当前last_pid值已经被使用
		if (task[i]->pid == last_pid || task[i]->pgrp == last_pid ||
		    task[i]->session == last_pid)
			goto repeat;
	}
	//如果所剩空闲进程空槽小于为root进程预留的MIN_TASKS_LEFT_FOR_ROOT（4）个，或者当前用户进程数大于系统限制的每个用户的进程数
	//则返回EAGAIN
	if (tasks_free <= MIN_TASKS_LEFT_FOR_ROOT ||
	    this_user_tasks > current->rlim[RLIMIT_NPROC].rlim_cur)
		//如果当前用户不是root用户
		if (current->uid)
			return -EAGAIN;
	return free_task;	//返回找到的空闲进程槽
}

//将old_file内容复制到新的文件描述符中，并返回新的文件描述符指针
static struct file * copy_fd(struct file * old_file)
{
	struct file * new_file = get_empty_filp();	//获得一项新的空的文件描述符
	int error;

	if (new_file) {
		memcpy(new_file,old_file,sizeof(struct file));	//复制
		new_file->f_count = 1;	//新的文件描述符引用计数设为1，因为新的文件描述符刚刚建立并指向一个文件
		if (new_file->f_inode)	//如果文件描述符真的指向的文件i节点存在，则将此文件i节点引用此时加1
			new_file->f_inode->i_count++;
		if (new_file->f_op && new_file->f_op->open) {
			error = new_file->f_op->open(new_file->f_inode,new_file);
			if (error) {
				iput(new_file->f_inode);
				new_file->f_count = 0;
				new_file = NULL;
			}
		}
	}
	return new_file;
}

//复制父进程的线性区和页表
static int dup_mmap(struct task_struct * tsk)
{
	struct vm_area_struct * mpnt, **p, *tmp;

	tsk->mm->mmap = NULL;
	p = &tsk->mm->mmap;
	for (mpnt = current->mm->mmap ; mpnt ; mpnt = mpnt->vm_next) {
		tmp = (struct vm_area_struct *) kmalloc(sizeof(struct vm_area_struct), GFP_KERNEL);
		if (!tmp) {
			exit_mmap(tsk);	//释放进程的地址空间
			return -ENOMEM;
		}
		*tmp = *mpnt;
		tmp->vm_task = tsk;
		tmp->vm_next = NULL;
		if (tmp->vm_inode) {
			tmp->vm_inode->i_count++;
			/* insert tmp into the share list, just after mpnt */
			tmp->vm_next_share->vm_prev_share = tmp;
			mpnt->vm_next_share = tmp;
			tmp->vm_prev_share = mpnt;
		}
		if (tmp->vm_ops && tmp->vm_ops->open)
			tmp->vm_ops->open(tmp);
		*p = tmp;
		p = &tmp->vm_next;
	}
	build_mmap_avl(tsk);
	return 0;
}

/*
	CLONE_FILES
	一个进程可能打开了一些文件，在进程结构task_struct中利用files（struct files_struct *）
	来保存进程打开的文件结构（struct file）信息，do_fork()中调用了copy_files()来处理这个进
	程属性；轻量级进程与父进程是共享该结构的，copy_files()时仅增加files->count计数。这一共
	享使得任何线程都能访问进程所维护的打开文件，对它们的操作会直接反映到进程中的其他线程。
*/

/*
 * SHAREFD not yet implemented..
 */
static void copy_files(unsigned long clone_flags, struct task_struct * p)
{
	int i;
	struct file * f;
	//COPYFD:set if fd's should be copied, not shared (NI)
	if (clone_flags & COPYFD) {
		for (i=0; i<NR_OPEN;i++)
			if ((f = p->files->fd[i]) != NULL)
				p->files->fd[i] = copy_fd(f);
	} else {
		for (i=0; i<NR_OPEN;i++)
			if ((f = p->files->fd[i]) != NULL)
				f->f_count++;
	}
}

/*
	CLONE_VM
	do_fork()需要调用copy_mm()来设置task_struct中的mm和active_mm项，这两个mm_struct
	数据与进程所关联的内存空间相对应。如果do_fork()时指定了CLONE_VM开关，copy_mm()将
	把新的task_struct中的mm和active_mm设置成与current的相同，同时提高该mm_struct的使
	用者数目（mm_struct::mm_users）。也就是说，轻量级进程与父进程共享内存地址空间
*/

/*
 * CLONEVM not yet correctly implemented: needs to clone the mmap
 * instead of duplicating it..
 */
static int copy_mm(unsigned long clone_flags, struct task_struct * p)
{
	if (clone_flags & COPYVM) {
		p->mm->min_flt = p->mm->maj_flt = 0;
		p->mm->cmin_flt = p->mm->cmaj_flt = 0;
		if (copy_page_tables(p))
			return 1;
		return dup_mmap(p);
	} else {
		if (clone_page_tables(p))
			return 1;
		return dup_mmap(p);		/* wrong.. */
	}
}

/*
	CLONE_FS
	task_struct中利用fs（struct fs_struct *）记录了进程所在文件系统的根目录和当前目录信息，
	do_fork()时调用copy_fs()复制了这个结构；而对于轻量级进程则仅增加fs->count计数，与父进程
	共享相同的fs_struct。也就是说，轻量级进程没有独立的文件系统相关的信息，进程中任何一个线程
	改变当前目录、根目录等信息都将直接影响到其他线程。
*/

static void copy_fs(unsigned long clone_flags, struct task_struct * p)
{
	if (current->fs->pwd)
		current->fs->pwd->i_count++;
	if (current->fs->root)
		current->fs->root->i_count++;
}

/*
	clone_flags：该标志位的4个字节分为两部分。最低的一个字节为子进程结束时发送给父进程的信号代码，
	通常为SIGCHLD；剩余的三个字节则是各种clone标志的组合
	regs：指向pt_regs结构体的指针。当系统发生系统调用，即用户进程从用户态切换到内核态时，该结构体
	保存通用寄存器中的值，并被存放于内核态的堆栈中
*/

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in its entirety.
 */
int do_fork(unsigned long clone_flags, unsigned long usp, struct pt_regs *regs)
{
	//这个变量被赋以find_empty_process()函数的返回值，
	//并用来表示新创建的子进程在task数组中的下标
	int nr;
	//这是用来标示新建子进程分给核心堆栈的内存空间的
	unsigned long new_stack;
	struct task_struct *p;

	//为task_struct结构分配内存空间
	if(!(p = (struct task_struct*)__get_free_page(GFP_KERNEL)))
		goto bad_fork;
	//为子进程分配新的系统堆栈空间
	new_stack = get_free_page(GFP_KERNEL);
	if (!new_stack)
		goto bad_fork_free;
	//为子进程分配新的PID
	nr = find_empty_process();
	if (nr < 0)
		goto bad_fork_free;

	//复制父进程的task_struct结构内容
	*p = *current;

	//如果父进程的执行域指针不为空，并且其use_count指针不为空，则增加此执行域的使用计数
	if (p->exec_domain && p->exec_domain->use_count)
		(*p->exec_domain->use_count)++;
	//相应的，增加父进程的二进制可执行程序的使用计数
	if (p->binfmt && p->binfmt->use_count)
		(*p->binfmt->use_count)++;

	//为0表示正在执行父进程的程序代码
	p->did_exec = 0;
	//指向新进程的内核态堆栈
	p->kernel_stack_page = new_stack;
	*(unsigned long *) p->kernel_stack_page = STACK_MAGIC;
	p->state = TASK_UNINTERRUPTIBLE;
	p->flags &= ~(PF_PTRACED|PF_TRACESYS);
	//设置进程的pid
	p->pid = last_pid;
	//将当前进程设置为此进程的父进程
	p->p_pptr = p->p_opptr = current;
	//此进程无子进程
	p->p_cptr = NULL;
	p->signal = 0;
	//此进程的内核间隔定时器
	p->it_real_value = p->it_virt_value = p->it_prof_value = 0;
	p->it_real_incr = p->it_virt_incr = p->it_prof_incr = 0;
	p->leader = 0;		/* process leadership doesn't inherit */
	p->tty_old_pgrp = 0;
	p->utime = p->stime = 0;
	p->cutime = p->cstime = 0;
	p->start_time = jiffies;
	//进程现在还不可以换出
	p->mm->swappable = 0;	/* don't try to swap it out before it's set up */
	task[nr] = p;
	//将进程链入系统进程链表中
	SET_LINKS(p);

	/* copy all the process information */
	//拷贝父进程的系统堆栈并做相应的调整
	copy_thread(nr, clone_flags, usp, p, regs);
	//copy_mm、copy_files和copy_fs会根据clone_flags标志来决定是复制还是共享父进程的vm、files和fs
	if (copy_mm(clone_flags, p))
		goto bad_fork_cleanup;
	//子进程的信号量的undo队列为空
	p->semundo = NULL;
	copy_files(clone_flags, p);
	copy_fs(clone_flags, p);

	/* ok, now we should be set up.. */
	//子进程现在可以换出了
	p->mm->swappable = 1;
	p->exit_signal = clone_flags & CSIGNAL;
	//子进程获取其父进程运行时间的一半
	p->counter = current->counter >> 1;
	//可以将子进程置为可运行状态了
	p->state = TASK_RUNNING;	/* do this last, just in case */
	return p->pid;
bad_fork_cleanup:
	task[nr] = NULL;
	REMOVE_LINKS(p);
bad_fork_free:
	free_page(new_stack);
	free_page((long) p);
bad_fork:
	return -EAGAIN;
}
