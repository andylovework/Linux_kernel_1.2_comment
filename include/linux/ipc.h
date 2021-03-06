#ifndef _LINUX_IPC_H
#define _LINUX_IPC_H
#include <linux/types.h>

typedef int key_t; 		/* should go in <types.h> type for IPC key */
#define IPC_PRIVATE ((key_t) 0)  

//Linux为每个IPC对象设置了一个ipc_perm结构体并在创建IPC对象的时候进行初始化。
//这个结构体中定义了IPC对象的访问权限和所有者:
struct ipc_perm
{
  key_t  key;
  ushort uid;   /* owner euid and egid */
  ushort gid;
  ushort cuid;  /* creator euid and egid */
  ushort cgid;
  ushort mode;  /* access modes see mode flags below */
  ushort seq;   /* sequence number *///这是系统保存的IPC对象的序列号 
									//详细的说明请参考msg.c中的相关注释以及《深入理解linux内核》第十九章关于资源标识符的说明
};


/* resource get request flags */
#define IPC_CREAT  00001000   /* create if key is nonexistent */
#define IPC_EXCL   00002000   /* fail if key exists */
#define IPC_NOWAIT 00004000   /* return error on wait */


/* 
 * Control commands used with semctl, msgctl and shmctl 
 * see also specific commands in sem.h, msg.h and shm.h
 */
#define IPC_RMID 0     /* remove resource */
#define IPC_SET  1     /* set ipc_perm options */
#define IPC_STAT 2     /* get ipc_perm options */
#define IPC_INFO 3     /* see ipcs */

#ifdef __KERNEL__

/* special shmsegs[id], msgque[id] or semary[id]  values */
#define IPC_UNUSED	((void *) -1)
#define IPC_NOID	((void *) -2)		/* being allocated/destroyed */

/* 
 * These are used to wrap system calls. See ipc/util.c.
 */
struct ipc_kludge {
    struct msgbuf *msgp;
    long msgtyp;
};

#define SEMOP	 	1
#define SEMGET 		2
#define SEMCTL 		3
#define MSGSND 		11
#define MSGRCV 		12
#define MSGGET 		13
#define MSGCTL 		14
#define SHMAT 		21
#define SHMDT 		22
#define SHMGET 		23
#define SHMCTL 		24

#define IPCCALL(version,op)	((version)<<16 | (op))

#endif /* __KERNEL__ */

#endif /* _LINUX_IPC_H */


