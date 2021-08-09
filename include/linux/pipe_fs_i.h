#ifndef _LINUX_PIPE_FS_I_H
#define _LINUX_PIPE_FS_I_H
struct pipe_inode_info {
	struct wait_queue * wait;	//不管是读进程还是写进程，都睡眠在同一个队列中，
								//我想这是为了在每次睡眠唤醒操作中，都对每个睡眠的进程做信号检测
	char * base;
	unsigned int start;
	unsigned int len;
	unsigned int lock;
	unsigned int rd_openers;	//以读方式打开此管道而被阻塞（因为没有写进程）的进程总数 也即睡眠在此管道节点上的读进程数
	unsigned int wr_openers;	//以写方式打开此管道而被阻塞（因为没有读进程）的进程总数 也即睡眠在此管道节点上的写进程数
	unsigned int readers;	//读进程总数
	unsigned int writers;	//写进程总数
};

#define PIPE_WAIT(inode)	((inode).u.pipe_i.wait)
#define PIPE_BASE(inode)	((inode).u.pipe_i.base)
#define PIPE_START(inode)	((inode).u.pipe_i.start)
#define PIPE_LEN(inode)		((inode).u.pipe_i.len)
#define PIPE_RD_OPENERS(inode)	((inode).u.pipe_i.rd_openers)
#define PIPE_WR_OPENERS(inode)	((inode).u.pipe_i.wr_openers)
#define PIPE_READERS(inode)	((inode).u.pipe_i.readers)
#define PIPE_WRITERS(inode)	((inode).u.pipe_i.writers)
#define PIPE_LOCK(inode)	((inode).u.pipe_i.lock)
#define PIPE_SIZE(inode)	PIPE_LEN(inode)

#define PIPE_EMPTY(inode)	(PIPE_SIZE(inode)==0)
#define PIPE_FULL(inode)	(PIPE_SIZE(inode)==PIPE_BUF)
#define PIPE_FREE(inode)	(PIPE_BUF - PIPE_LEN(inode))
#define PIPE_END(inode)		((PIPE_START(inode)+PIPE_LEN(inode))&\
							   (PIPE_BUF-1))
#define PIPE_MAX_RCHUNK(inode)	(PIPE_BUF - PIPE_START(inode))
#define PIPE_MAX_WCHUNK(inode)	(PIPE_BUF - PIPE_END(inode))

#endif
