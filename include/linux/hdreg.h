#ifndef _LINUX_HDREG_H
#define _LINUX_HDREG_H

#include <linux/config.h>

/*
 * This file contains some defines for the AT-hd-controller.
 * Various sources. Check out some definitions (see comments with
 * a ques).
 */

/*
	cpu的主要作用之一就是控制设备之间的数据交互。这其中自然也包括了硬盘。系统的所有数据基本
	都在硬盘中，所以知道怎么读写硬盘，对程序来说非常重要，所以我们先来探索下传说中的pio模式。
	cpu要想直接访问设备里的数据，必须对设备存储空间进行编址。而硬盘数据数据太大，直接编址数
	据线成本太高，于是设计上在这类设备和总线之间加了一个控制器。这个控制器里有少量寄存器可以
	被cpu访问，而这个控制器又能控制硬盘驱动器读写数据，所以，cpu通过对硬盘控制器里的少量io寄
	存器的读写来控制对整个硬盘的读写。cpu告诉磁盘控制器读哪些地址的数据，磁盘控制器读好之后放
	入能被 cpu访问的数据寄存器中，再将这里的数据传给内存，这确实是个不错的办法。
	首先，我们先找到硬盘控制器io端口地址的分配，
	0×170 — 0×177 IDE 硬盘控制器 1	0001 0111 0000 -- 0001 0111 0111
	0x1F0 — 0x1F7 IDE 硬盘控制器 0。 0001 1111 0000 -- 0001 1111 0111

	由HD_CURRENT的定义可知， 101dhhhh , d=drive, hhhh=head 
	101表示ECC校检和每扇区512B d表示驱动器(0/1)所以IDE一个接口最多支持两个驱动器 h表示磁头号
	这里我的理解是，IDE有两个接口，每个接口其实占据了一个io端口0×170 — 0×177或0x1F0 — 0x1F7
	也即两个硬盘控制器。而对于一个控制器而言，由HD_CURRENT的d位定义可知，其可以控制两个硬盘设备0/1
	在本文件中，内核之针对一个硬盘控制器进行了编程定义，即端口0x1F0 — 0x1F7，而没有对另一个端口进行
	编程定义。原因在这里不想探讨，具体详细的IDE驱动程序请参见ide.c文件。
*/

/*
	硬盘读写是一个复杂的过程，它涉及到硬盘的接口方式、寻址方式、控制寄存器模型等。硬盘
	的存储介质经历了从磁性材料、光磁介质到Flash半导体存储材料，对它们的读写方法和寻址方式都
	一样，因为这些存储介质与计算机的接口共同遵循着ATA标准。主机与硬盘之间的数据传输按程序
	I/O或DMA方式进行，硬盘的寻址方式可按CHS或LBA。在计算机应用中，掌握硬盘读写技术很有
	必要，  像UNIX系统的dd命令和目前流行的Ghost、DiskEdit等软件，都可以把数十个GB容量硬盘
	上庞大的软件系统，在短时间内复制完成。这些工具软件的构造正是基于该技术而设计的。本文从
	IDE控制器的寄存器模型入手，分析硬盘的读写方法和寻址方式，结合实例剖析了这类复杂硬盘工
	具软件的设计思路及制作方法。
*/

/*
	IDE控制器的寄存器模型
	计算机主机对IDE接口硬盘的控制是通过硬盘控制器上的二组寄存器实现[1]
	。一组为命令寄存器组(Task File Registers)， I/O的端口地址为1F0H~1F7H， 
	其作用是传送命令与命令参数， 如表1所示。
	另一组为控制/诊断寄存器(Control/Diagnostic Registers)，I/O的端口地址为3F6H~3F7H，其作用是控
	制硬盘驱动器，如表2所示。
*/

/*
	表1   Task File Registers	命令寄存器组
	I/O地址  读(主机从硬盘读数据)  写(主机数据写入硬盘)
	1F0H  数据寄存器  数据寄存器
	1F1H  错误寄存器(只读寄存器)  特征寄存器
	1F2H  扇区计数寄存器  扇区计数寄存器
	1F3H  扇区号寄存器或LBA块地址0~7  扇区号或LBA块地址0~7
	1F4H  磁道数低8位或LBA块地址8~15  磁道数低8位或LBA块地址8~15
	1F5H  磁道数高8位或LBA块地址16~23  磁道数高8位或LBA块地址16~23
	1F6H  驱动器/磁头或LBA块地址24~27  驱动器/磁头或LBA块地址24~27
	1F7H  状态寄存器  命令寄存器 
*/
/* Hd controller regs. Ref: IBM AT Bios-listing */
/* For a second IDE interface, xor all addresses with 0x80 */
#define HD_DATA		0x1f0	/* _CTL when writing */
#define HD_ERROR	0x1f1	/* see err-bits */
#define HD_NSECTOR	0x1f2	/* nr of sectors to read/write */
#define HD_SECTOR	0x1f3	/* starting sector */
#define HD_LCYL		0x1f4	/* starting cylinder */
#define HD_HCYL		0x1f5	/* high byte of starting cyl */
#define HD_CURRENT	0x1f6	/* 101dhhhh , d=drive, hhhh=head */
				//101表示ECC校检和每扇区512B d表示驱动器(0/1)所以IDE一个接口最多支持两个驱动器 h表示磁头号
#define HD_STATUS	0x1f7	/* see status-bits */
#define HD_FEATURE HD_ERROR	/* same io address, read=error, write=feature */
#define HD_PRECOMP HD_FEATURE	/* obsolete use of this port - predates IDE */
#define HD_COMMAND HD_STATUS	/* same io address, read=status, write=cmd */
//表2 Control/Diagnostic Registers控制/诊断寄存器
//I/O地址  读  写
//3F6H  交换状态寄存器(只读寄存器)  设备控制寄存器(复位)
//3F7H  驱动器地址寄存器  
#define HD_CMD		0x3f6	/* used for resets */
#define HD_ALTSTATUS	0x3f6	/* same as HD_STATUS but doesn't clear irq */

/*
	在硬盘执行读写过程中，为了节省I/O地址空间，用相同的地址来标识不同的寄存器。例如，
	如表1中端口地址1F7H，在向硬盘写入数据时作为命令寄存器，而向硬盘读取数据时作为状态寄存
	器。表1中各寄存器功能如下：
	数据寄存器：是主机和硬盘控制器的缓冲区之间进行8位或16位数据交换用的寄存器，使用该
	寄存器进行数据传输的方式称程序输入输出方式，即PIO方式，数据交换的另一种方式是通过DMA
	通道，这种方式不使用数据寄存器进行数据交换；
	错误寄存器：该寄存器包含了上次命令执行后硬盘的诊断信息。每位意义见表3，在启动系统、
	硬盘复位或执行硬盘的诊断程序后，也在该寄存器中保存着一个诊断码。
*/

/*
	IDE状态寄存器位意义
	0  ERR，错误(ERROR)，该位为1表示在结束前次的命令执行时发生了无法恢复的
	错误。在错误寄存器中保存了更多的错误信息。
	1  IDX，反映从驱动器读入的索引信号。
	2  CORR，该位为1时，表示已按ECC算法校正硬盘的读数据。
	3  DRQ，为1表示请求主机进行数据传输(读或写)。
	4  DSC，为1表示磁头完成寻道操作，已停留在该道上。
	5  DF，为1时，表示驱动器发生写故障。
	6  DRDY，为1时表示驱动器准备好，可以接受命令。
	7  BSY，为1时表示驱动器忙(BSY)，正在执行命令。在发送命令前先判断该位
*/
/* Bits of HD_STATUS */
#define ERR_STAT	0x01
#define INDEX_STAT	0x02
#define ECC_STAT	0x04	/* Corrected error */
/*DRQ_STAT：数据请求服务。当该位被置位时，表示驱动器已经准备好在主机和数据端口之间传输一个字或一个字节的数据*/
#define DRQ_STAT	0x08
#define SEEK_STAT	0x10
#define WRERR_STAT	0x20
/*驱动器准备就绪。表示驱动器已经准备好*/
#define READY_STAT	0x40
/*控制器忙碌。当驱动器正在操作时由驱动器的控制位设置该位。此时主机不能发送命令块。而对任何命令寄存器的读操作
将返回状态寄存器的值。*/
#define BUSY_STAT	0x80

/* Values for HD_COMMAND */
#define WIN_RESTORE		0x10
#define WIN_READ		0x20
#define WIN_WRITE		0x30
#define WIN_VERIFY		0x40
#define WIN_FORMAT		0x50
#define WIN_INIT		0x60
#define WIN_SEEK 		0x70
#define WIN_DIAGNOSE		0x90
#define WIN_SPECIFY		0x91
#define WIN_SETIDLE1		0xE3
#define WIN_SETIDLE2		0x97

#define WIN_PIDENTIFY		0xA1	/* identify ATA-PI device	*/
#define WIN_MULTREAD		0xC4	/* read multiple sectors	*/
#define WIN_MULTWRITE		0xC5	/* write multiple sectors	*/
#define WIN_SETMULT		0xC6	/* enable read multiple		*/
#define WIN_IDENTIFY		0xEC	/* ask drive to identify itself	*/
#define WIN_SETFEATURES		0xEF	/* set special drive features   */

/* Bits for HD_ERROR */
#define MARK_ERR	0x01	/* Bad address mark */
#define TRK0_ERR	0x02	/* couldn't find track 0 */
#define ABRT_ERR	0x04	/* Command aborted */
#define ID_ERR		0x10	/* ID field not found */
#define ECC_ERR		0x40	/* Uncorrectable ECC error */
#define	BBD_ERR		0x80	/* block marked bad */

struct hd_geometry {
      unsigned char heads;
      unsigned char sectors;
      unsigned short cylinders;
      unsigned long start;
};

/* hd/ide ctl's that pass (arg) ptrs to user space are numbered 0x30n/0x31n */
#define HDIO_GETGEO		0x301	/* get device geometry */
#define HDIO_REQ		HDIO_GETGEO	/* obsolete, use HDIO_GETGEO */
#define HDIO_GET_UNMASKINTR	0x302	/* get current unmask setting */
#define HDIO_GET_MULTCOUNT	0x304	/* get current IDE blockmode setting */
#define HDIO_GET_IDENTITY 	0x307	/* get IDE identification info */
#define HDIO_GET_KEEPSETTINGS 	0x308	/* get keep-settings-on-reset flag */
#define HDIO_DRIVE_CMD		0x31f	/* execute a special drive command */

/* hd/ide ctl's that pass (arg) non-ptr values are numbered 0x32n/0x33n */
#define HDIO_SET_MULTCOUNT	0x321	/* set IDE blockmode */
#define HDIO_SET_UNMASKINTR	0x322	/* permit other irqs during I/O */
#define HDIO_SET_KEEPSETTINGS	0x323	/* keep ioctl settings on reset */

/*
	IDE硬盘均提供有ATA自动识别功能,由这些功能可以读到一个512bytes大小的硬盘信息
	参数表，它包括有硬盘磁头数，柱面数，扇区数，系列号及其它一些厂家的指标,从而
	使系统BIOS可以自动的读取IDE硬盘的磁头数，柱面数，扇区数等信息并保存之。
*/
/* structure returned by HDIO_GET_IDENTITY, as per ANSI ATA2 rev.2f spec */
struct hd_driveid {
	unsigned short	config;		/* lots of obsolete bit flags */
	unsigned short	cyls;		/* "physical" cyls */
	unsigned short	reserved2;	/* reserved (word 2) */
	unsigned short	heads;		/* "physical" heads */
	unsigned short	track_bytes;	/* unformatted bytes per track */
	unsigned short	sector_bytes;	/* unformatted bytes per sector */
	unsigned short	sectors;	/* "physical" sectors per track */
	unsigned short	vendor0;	/* vendor unique */
	unsigned short	vendor1;	/* vendor unique */
	unsigned short	vendor2;	/* vendor unique */
	unsigned char	serial_no[20];	/* 0 = not_specified *///serial_no为硬盘的序列号.如果此项为0,则为没有提供.
	unsigned short	buf_type;
	unsigned short	buf_size;	/* 512 byte increments; 0 = not_specified */
	unsigned short	ecc_bytes;	/* for r/w long cmds; 0 = not_specified */
	unsigned char	fw_rev[8];	/* 0 = not_specified */
	unsigned char	model[40];	/* 0 = not_specified */
	unsigned char	max_multsect;	/* 0=not_implemented */
	unsigned char	vendor3;	/* vendor unique */
	unsigned short	dword_io;	/* 0=not_implemented; 1=implemented */
	unsigned char	vendor4;	/* vendor unique */
	unsigned char	capability;	/* bits 0:DMA 1:LBA 2:IORDYsw 3:IORDYsup*/
	unsigned short	reserved50;	/* reserved (word 50) */
	unsigned char	vendor5;	/* vendor unique */
	unsigned char	tPIO;		/* 0=slow, 1=medium, 2=fast */
	unsigned char	vendor6;	/* vendor unique */
	unsigned char	tDMA;		/* 0=slow, 1=medium, 2=fast */
	unsigned short	field_valid;	/* bits 0:cur_ok 1:eide_ok */
	unsigned short	cur_cyls;	/* logical cylinders */
	unsigned short	cur_heads;	/* logical heads */
	unsigned short	cur_sectors;	/* logical sectors per track */
	unsigned short	cur_capacity0;	/* logical total sectors on drive */
	unsigned short	cur_capacity1;	/*  (2 words, misaligned int)     */
	unsigned char	multsect;	/* current multiple sector count */
	unsigned char	multsect_valid;	/* when (bit0==1) multsect is ok */
	unsigned int	lba_capacity;	/* total number of sectors */
	unsigned short	dma_1word;	/* single-word dma info */
	unsigned short	dma_mword;	/* multiple-word dma info */
	unsigned short  eide_pio_modes; /* bits 0:mode3 1:mode4 */
	unsigned short  eide_dma_min;	/* min mword dma cycle time (ns) */
	unsigned short  eide_dma_time;	/* recommended mword dma cycle time (ns) */
	unsigned short  eide_pio;       /* min cycle time (ns), no IORDY  */
	unsigned short  eide_pio_iordy; /* min cycle time (ns), with IORDY */
	unsigned short  reserved69;	/* reserved (word 69) */
	unsigned short  reserved70;	/* reserved (word 70) */
	/* unsigned short reservedxx[57];*/	/* reserved (words 71-127) */
	/* unsigned short vendor7  [32];*/	/* vendor unique (words 128-159) */
	/* unsigned short reservedyy[96];*/	/* reserved (words 160-255) */
};
/*
http://bbs.chinaunix.net/thread-677910-1-1.html
在网上一大把关于如何获取硬盘物理参数的文章.
我已经在网上找了N久,找出以下两种方法能得到硬盘的参数.

(一) .通过对IDE的I/O端口(1F0-1F7)操作获得硬盘参数.

①向端口3F6写入控制字节，建立相应的硬盘控制方式；
②检验硬盘控制器和驱动器的状态(检测端口的第7和第6两位)，如果控制器空闲而且驱动器就绪，即可输入命令；
③完整的输入7个字节长度的命令块，一次写入端口1F1H-1F7H，不论是否需要，端口1F1H-1F6H对应的前6个字节的参数必须读出，端口1F7H的输出命令码为“0ECH”；
④检测端口1F7H的第7和第3两位，如果控制器空闲且第3位置1，表示操作结束，即可读取结果；
⑤通过端口1F0H读取100H字节到缓冲区；
⑥再次读取端口1F7H，判断第0位是否为0，如果为0，表示命令成功，否则表示命令失败；

读出的256字节信息的主要内容如下：
┏━━━━┯━━━━━━━━━┯━━━━━┓
┃ 偏移量 │ 内 容             │长度(字节)┃
┠────┼─────────┼─────┨
┃02H      │柱面数            │2        ┃
┃06H      │磁头数            │2        ┃
┃08H      │每磁道所含的字节数 │2        ┃
┃0AH      │没扇区所含的字节数 │2        ┃
┃0CH      │每磁道所含的扇区数 │2        ┃
┃14H      │产品的序列号       │20       ┃  
┃2AH      │硬盘缓冲区容量     │2        ┃
┃2CH      │ECC校验码的长度   │2        ┃
┃2EH      │硬件修正号         │8        ┃
┃36H      │硬盘型号           │4       ┃
┗━━━━┷━━━━━━━━━┷━━━━━┛
以为这个似乎能解决了,但其实只是读出了 C.H.S 和硬盘序列号而以,先不说能访问8.4G以的磁盘空间.现在的电脑的BIOS都能自己检测硬盘,COMS->AUTO.
这样以来,硬盘是以LBA的方式工作,而不是C.H.S方式工作.
如果我们用BIOSDISK()这个函数再加上以上方式得到的C.H.S参数来用的话.就会出错.所以要得到硬盘的LBA参数.

(二)通过调用中断INT13 AH=48H(扩展)来获取硬盘LBA参数

入口:
    AH = 48h
    DL = 驱动器号
    DS:SI = 返回数据缓冲区地址
返回:
    CF = 0, AH = 0 成功
    DS:SI 硬盘参数数据包地址
    CF = 1, AH = 错误码
*/
/*
 * These routines are used for kernel command line parameters from main.c:
 */
#ifdef CONFIG_BLK_DEV_HD
void hd_setup(char *, int *);
#endif	/* CONFIG_BLK_DEV_HD */
#ifdef CONFIG_BLK_DEV_IDE
void ide_setup(char *, int *);
void hda_setup(char *, int *);
void hdb_setup(char *, int *);
void hdc_setup(char *, int *);
void hdd_setup(char *, int *);
#endif	/* CONFIG_BLK_DEV_IDE */

#endif	/* _LINUX_HDREG_H */
