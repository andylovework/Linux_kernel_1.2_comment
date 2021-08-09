#ifndef _LINUX_BLKDEV_H
#define _LINUX_BLKDEV_H

#include <linux/major.h>
#include <linux/sched.h>
#include <linux/genhd.h>

/*
 * Ok, this is an expanded form so that we can use the same
 * request for paging requests when that is implemented. In
 * paging, 'bh' is NULL, and the semaphore is used to wait
 * for read/write completion.
 */
 //linux内核中，使用struct request来表示等待处理的块设备IO请求
struct request {
	int dev;		/* -1 if no request */
	int cmd;		/* READ or WRITE */
	int errors;
	unsigned long sector;	//要操作的首个扇区
	unsigned long nr_sectors;	//请求项中缓冲区链表中所有缓冲区所对应的扇区总数
	unsigned long current_nr_sectors;	//当前要操作的缓冲区所对应的的扇区数
	char * buffer;	//存放buffer_head.b_data值，表示发出请求的数据存取地址
	struct semaphore * sem;	//一个信号量，用来保证设备读写的原语操作，当sem=0时才能处理该请求
	struct buffer_head * bh;	//读写缓冲区链表的头指针 注：缓冲区链表中的缓冲区对应的扇区编号是相邻递增的
	struct buffer_head * bhtail;	//读写缓冲区链表的尾指针
	struct request * next;	//指向下一个请求
};

struct blk_dev_struct {
	void (*request_fn)(void);	//指向请求处理函数的指针，请求处理函数是写设备驱动程序的重要一环，
					//设备驱动程序在此函数中通过outb向位于I/O空间中的设备命令寄存器发出命令
	struct request * current_request;	//指向当前正在处理的请求
};

struct sec_size {
	unsigned block_size;
	unsigned block_size_bits;
};

extern struct sec_size * blk_sec[MAX_BLKDEV];
extern struct blk_dev_struct blk_dev[MAX_BLKDEV];
extern struct wait_queue * wait_for_request;
extern void resetup_one_dev(struct gendisk *dev, int drive);

//定义于\drivers\block\ll_rw_blk.c
extern int * blk_size[MAX_BLKDEV];

extern int * blksize_size[MAX_BLKDEV];

extern int * hardsect_size[MAX_BLKDEV];

#endif
