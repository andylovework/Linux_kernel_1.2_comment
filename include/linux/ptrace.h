#ifndef _LINUX_PTRACE_H
#define _LINUX_PTRACE_H
/* ptrace.h */
/* structs and defines to help the user use the ptrace system call. */

/* has the defines to get at the registers. */

//本进程被其父进程所跟踪。其父进程应该希望跟踪子进程。
#define PTRACE_TRACEME		   0
//从内存地址中读取一个字节
#define PTRACE_PEEKTEXT		   1
//用PTRACE_POKEDATA作为第一个参数，以此来改变子进程中的变量值。它以与PTRACE_PEEKDATA相似的方式工作，
//当然，它不只是偷窥变量的值了，它可以修改它们。
#define PTRACE_PEEKDATA		   2
//通过将PTRACE_PEEKUSER作为ptrace 的第一个参数进行调用，可以取得与子进程相关的寄存器值。
#define PTRACE_PEEKUSR		   3
//往内存地址中写入一个字节。
#define PTRACE_POKETEXT		   4
#define PTRACE_POKEDATA		   5
//往USER区域中写入一个字节
#define PTRACE_POKEUSR		   6
//我们察看完系统调用的信息后，可以使用PTRACE_CONT作为ptrace的第一个参数，调用ptrace使子进程继续系统调用的过程。
#define PTRACE_CONT		   7
//杀掉子进程，使它退出。
#define PTRACE_KILL		   8
//ptrace提供了对子进程进行单步的功能。 ptrace(PTRACE_SINGLESTEP, …) 会使内核在子进程的每一条指令执行前先将其阻塞，
//然后将控制权交给父进程。
#define PTRACE_SINGLESTEP	   9

//跟踪指定pid 进程。
#define PTRACE_ATTACH		0x10
//结束跟踪
#define PTRACE_DETACH		0x11

//使用PTRACE_SYSCALL作为ptrace的第一个参数，使内核在子进程做出系统调用或者准备退出的时候暂停它。
///这种行为与使用PTRACE_CONT，然后在下一个系统调用/进程退出时暂停它是等价的。
#define PTRACE_SYSCALL		  24

#include <asm/ptrace.h>

#endif
