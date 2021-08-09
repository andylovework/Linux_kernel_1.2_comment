#ifndef _LINUX_GENHD_H
#define _LINUX_GENHD_H

/*
 * 	genhd.h Copyright (C) 1992 Drew Eckhardt
 *	Generic hard disk header file by  
 * 		Drew Eckhardt
 *
 *		<drew@colorado.edu>
 */
	
#define EXTENDED_PARTITION 5
	
struct partition {
	unsigned char boot_ind;		/* 0x80 - active */
	unsigned char head;		/* starting head */
	unsigned char sector;		/* starting sector */
	unsigned char cyl;		/* starting cylinder */
	unsigned char sys_ind;		/* What partition type */
	unsigned char end_head;		/* end head */
	unsigned char end_sector;	/* end sector */
	unsigned char end_cyl;		/* end cylinder */
	unsigned int start_sect;	/* starting sector counting from 0 */
	unsigned int nr_sects;		/* nr of sectors in partition */
};

struct hd_struct {
	long start_sect;	// 起始扇区号
	long nr_sects;	 //总扇区数
};
/*
	结构体gendisk代表了一个通用硬盘（generic hard disk）对象，
	它存储了一个硬盘的信息，包括请求队列、分区链表和块设备操作
	函数集等。块设备驱动程序分配结构gendisk实例，装载分区表，
	分配请求队列并填充结构的其他域。
*/
struct gendisk {
	int major;			/* major number of driver *//* 确定这个结构所指的设备驱动程序的主设备号 */  
	char *major_name;		/* name of major driver */
					/*
					属于这个主设备号的设备的基本名。每个设备名是通过在这个名字后为每个单元加一个
					字母并为每个分区加一个数字得到。例如，“hd”是用来构成/dev/hda1和/dev/hda3的基
					本名。基本名最多5个字符长，因为add_partition在一个8字节的缓冲区中构造全名，它
					要附加上一个确定单元的字母，分区号和一个终止符‘\0’。
					*/
	int minor_shift;		/* number of times minor is shifted to get real minor */
					/*
					从设备的次设备号中获取驱动器号要进行移位的次数。这个域中的值
					应与宏DEVICE_NR(device)中的定义一致（见“头文件blk.h”）
					*/
	int max_p;			/* maximum partitions per device *///分区的最大数目
	int max_nr;			/* maximum number of real devices */
					/*
					单元的最大数目。单元最大数目在移位minor_shift次后的结果应匹配次设备号的可能的范围，
					目前是0-255。IDE驱动程序可以同时支持很多驱动器和每一个驱动器很多分区，因为它注册了
					几个主设备号，从而绕过了次设备号范围小的问题。
					*/
	void (*init)(void);		/* Initialization called before we do our thing */
					//驱动程序的初始化函数，它在初始化设备后和分区检查执行前被调用
	struct hd_struct *part;		/* partition table */
					/*
					设备解码后的分区表。驱动程序用这一项确定通过每个次设备号哪些范围的磁盘扇区是可以
					访问的。大多数驱动程序实现max_nr<<minor_shift个结构的静态数值，并负责数组的分配和
					释放。在核心解码分区表之前驱动程序应将数组初始化为零。
					*/
	int *sizes;			/* size of device in blocks */
					/*
					 这个域指向一个整数数组。这个数组保持着与blk_size同样的信息。驱动程序负责分配和释放该
					数据区域。注意设备的分区检查把这个指针拷贝到blk_size，因此处理可分区设备的驱动程序不必
					分配这后一个数组。
					*/
	int nr_real;			/* number of real devices */
					// 存在的真实设备（单元）的个数。这个数字必须小于等于max_nr。
	void *real_devices;		/* internal use */
					//这个指针被那些需要保存一些额外私有信息的驱动程序内部使用（这与filp->private_data类似）。
	struct gendisk *next;		//在普通硬盘列表中的一根链。
};

extern int NR_GENDISKS;			/* total */
extern struct gendisk *gendisk_head;	/* linked list of disks */

#endif
