/*
 *  linux/drivers/block/ll_rw_blk.c
 *
 * Copyright (C) 1991, 1992 Linus Torvalds
 * Copyright (C) 1994,      Karl Keyte: Added support for disk statistics
 */

/*
 * This handles all read/write requests to block devices
 */
 /*
	Linux内核正是通过这个文件所提供的界面，来向系统内的其他子系统
	屏蔽了底层真实的物理块设备的读写，提供了统一的块设备读写界面，
	即ll_rw_xxxx()接口。
	
	也正是因为这里所定义的向底层物理设备的读写请求是建立在bh缓冲之
	上的，所以内核内所有的文件操作都是建立在bh缓冲之上的。当上层文
	件系统要读写某个物理设备的数据时，它首先在bh中需找其所需要的数
	据是否有效的存在于bh中，若是，则直接从bh中读取，若不存在，则需
	要向底层物理设备读写文件数据了。但在读写之前，必须先申请获得一
	个bh缓冲（getblk()），并将其传递给底层读写请求队列，底层读写程
	序会将物理设备上的指定数据读入到这些bh缓冲中，供上层系统使用，
	并提供缓存机制，以提高系统效率。
	
	从这里也可以看出，物理设备的真实块大小其实是其扇区大小，即512B
	设备的逻辑块大小是扇区大小的2的幂次的倍数。
	
	真实的块设备的读写例程是保存在其struct blk_dev_struct中的do_request()
	函数中的。不同的块设备具有不同的主设备号，系统也正是以其主设备号为索引
	找到其struct blk_dev_struct结构并调用真实的读写函数的。值得说明的是
	块设备文件呢？块设备文件也具有“设备号”，但其本质仍然是文件，所以读写
	块设备文件仍是通过文件系统，通过其文件名和inode来访问的。其inode中
	标明了其所属的真实设备的主设备号，并以此来建立读写请求、访问真实设备
	的读写驱动程序。通过getblk()函数获取的bh中保存有其代表的设备号。
	
	各种类型的块设备都是在内核静态分配主设备号的，代表系统支持的所有类型
	的块设备数组blk_dev在各个块设备驱动文件中由块设备自己注册。其使用的
	主设备号由内核静态分配，是固定的，定义在linux/include/linux/major.h
	
	在文件系统的超级块中保存有其所属的块设备号，并继承给其下的目录、文件
	如此建立起了文件系统大厦底层读写的基础和依据
 */
/*
	<深入分析linux内核>
	1．扇区及块缓冲区

	块设备的每次数据传送操作都作用于一组相邻字节，我们称之为扇区。
	在大部分磁盘设备中，扇区的大小是512字节，但是现在新出现的一些
	设备使用更大的扇区（1024和2014字节）。注意，应该把扇区作为数据
	传送的基本单元：不允许传送少于一个扇区的数据，而大部分磁盘设备
	都可以同时传送几个相邻的扇区。

	所谓块就是块设备驱动程序在一次单独操作中所传送的一大块相邻字节。
	注意不要混淆块（block）和扇区（sector）：扇区是硬件设备传送数
	据的基本单元，而块只是硬件设备请求一次I/O操作所涉及的一组相邻字节。

	在Linux中，块大小必须是2的幂，而且不能超过一个页面。此外，它必
	须是扇区大小的整数倍，因为每个块必须包含整数个扇区。因此，在PC
	体系结构中，允许块的大小为512、1024、2048和4096字节。同一个块
	设备驱动程序可以作用于多个块大小，因为它必须处理共享同一主设备
	号的一组设备文件，而每个块设备文件都有自己预定义的块大小。例如，
	一个块设备驱动程序可能会处理有两个分区的硬盘，一个分区包含Ext2
	文件系统，另一个分区包含交换分区。

	内核在一个名为blksize_size的表中存放块的大小；表中每个元素的索
	引就是相应块设备文件的主设备号和次设备号。如果blksize_size[M]
	为NULL，那么共享主设备号M的所有块设备都使用标准的块大小，即1024字节。

	每个块都需要自己的缓冲区，它是内核用来存放块内容的RAM内存区。当
	设备驱动程序从磁盘读出一个块时，就用从硬件设备中所获得的值来填充
	相应的缓冲区；同样，当设备驱动程序向磁盘中写入一个块时，就用相关
	缓冲区的实际值来更新硬件设备上相应的一组相邻字节。缓冲区的大小一
	定要与块的大小相匹配。
	
	在VFS直接访问某一块设备上的特定块时，也会触发缓冲区I/O操作。例如，
	如果内核必须从磁盘文件系统中读取一个索引节点，那么它必须从相应磁
	盘分区的块中传送数据 。对于特定块的直接访问是由bread()和breada()
	函数来执行的，这两个函数又会调用前面提到过的getblk()和ll_rw_block()函数。

	由于块设备速度很慢，因此缓冲区I/O数据传送通常都是异步处理的：低级
	设备驱动程序对DMAC和磁盘控制器进行编程来控制其操作，然后结束。当数
	据传送完成时，就会产生一个中断，从而第二次激活这个低级设备驱动程序
	来清除这次I/O操作所涉及的数据结构。

	3．块设备请求

	虽然块设备驱动程序可以一次传送一个单独的数据块，但是内核并不会为磁
	盘上每个被访问的数据块都单独执行一次I/O操作：这会导致磁盘性能的下降，
	因为确定磁盘表面块的物理位置是相当费时的。取而代之的是，只要可能，
	内核就试图把几个块合并在一起，并作为一个整体来处理，这样就减少了磁
	头的平均移动时间。

	当进程、VFS层或者任何其他的内核部分要读写一个磁盘块时，就真正引起一
	个块设备请求。从本质上说，这个请求描述的是所请求的块以及要对它执行的
	操作类型（读还是写）。然而，并不是请求一发出，内核就满足它，实际上，
	块请求发出时I/O操作仅仅被调度，稍后才会被执行。这种人为的延迟有悖于
	提高块设备性能的关键机制。当请求传送一个新的数据块时，内核检查能否通
	过稍微扩大前一个一直处于等待状态的请求而满足这个新请求，也就是说，能
	否不用进一步的搜索操作就能满足新请求。由于磁盘的访问大都是顺序的，因
	此这种简单机制就非常高效。

	延迟请求复杂化了块设备的处理。例如，假设某个进程打开了一个普通文件，
	然后，文件系统的驱动程序就要从磁盘读取相应的索引节点。高级块设备驱
	动程序把这个请求加入一个等待队列，并把这个进程挂起，直到存放索引节
	点的块被传送为止。

	因为块设备驱动程序是中断驱动的，因此，只要高级驱动程序一发出块请求，
	它就可以终止执行。在稍后的时间低级驱动程序才被激活，它会调用一个所
	谓的策略程序从一个队列中取得这个请求，并向磁盘控制器发出适当的命令
	来满足这个请求。当I/O操作完成时，磁盘控制器就产生一个中断，如果需要，
	相应的处理程序会再次调用这个策略程序来处理队列中进程的下一个请求。

	每个块设备驱动程序都维护自己的请求队列；每个物理块设备都应该有一个请
	求队列，以提高磁盘性能的方式对请求进行排序。因此策略程序就可以顺序扫
	描这种队列，并以最少地移动磁头而为所有的请求提供服务。


*/
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/config.h>
#include <linux/locks.h>
#include <linux/mm.h>

#include <asm/system.h>
#include <asm/io.h>
#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
 /*
	对不同块设备的所有请求都放在请求数组all_requests中，
	该数组实际上是一个请求缓冲池，请求的释放与获取都是
	针对这个缓冲池进行；同时各个设备的请求用next指针联
	结起来，形成各自的请求队列
	
	每个块设备中当前请求项与请求项数组中该设备的请求项
	链表共同构成了当前块设备的请求队列,此队列由
	struct blk_dev_struct结构中的current_request指针指向
	
	对于一个当前空闲的块设备，当ll_rw_block为其建立第一
	个请求项时，会让当前请求项指针指向刚刚创建的请求项，
	并立刻调用当前块设备的请求项操作函数进行读写操作
 */
//NR_REQUEST=64 defined in linux/drivers/block/blk.h
//struct request定义于linux/include/linux/blkdev.h
static struct request all_requests[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct wait_queue * wait_for_request = NULL;

/* This specifies how many sectors to read ahead on the disk.  */
//数组的每一项决定了特定的块设备预读的扇区数
int read_ahead[MAX_BLKDEV] = {0, };

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
//所有块设备的描述符都存放在blk_dev表中： 
/*
	每个块设备都对应着数组中的一项，可以用主设备号进行检索。每当用户进程对一个块
	设备发出一个读写请求时，首先调用块设备所公用的函数 generic_file_read ()和
	generic_file_write ()，如果数据存在缓冲区中或缓冲区还可以存放数据，就同缓
	冲区进行数据交换。否则,系统会将相应的请求队列结构添加到其对应项的 blk_dev_struct 中，
	如果在加入请求队列结构的时候该设备没有请求的话，则马上响应该请求，否则将其追加到请求任务队列尾顺序执行。 
*/
struct blk_dev_struct blk_dev[MAX_BLKDEV] = {
	{ NULL, NULL },		/* 0 no_dev */
	{ NULL, NULL },		/* 1 dev mem */
	{ NULL, NULL },		/* 2 dev fd */
	{ NULL, NULL },		/* 3 dev ide0 or hd */
	{ NULL, NULL },		/* 4 dev ttyx */
	{ NULL, NULL },		/* 5 dev tty */
	{ NULL, NULL },		/* 6 dev lp */
	{ NULL, NULL },		/* 7 dev pipes */
	{ NULL, NULL },		/* 8 dev sd */
	{ NULL, NULL },		/* 9 dev st */
	{ NULL, NULL },		/* 10 */
	{ NULL, NULL },		/* 11 */
	{ NULL, NULL },		/* 12 */
	{ NULL, NULL },		/* 13 */
	{ NULL, NULL },		/* 14 */
	{ NULL, NULL },		/* 15 */
	{ NULL, NULL },		/* 16 */
	{ NULL, NULL },		/* 17 */
	{ NULL, NULL },		/* 18 */
	{ NULL, NULL },		/* 19 */
	{ NULL, NULL },		/* 20 */
	{ NULL, NULL },		/* 21 */
	{ NULL, NULL }		/* 22 dev ide1 */
};

/*
 * blk_size contains the size of all block-devices in units of 1024 byte
 * sectors:
 *
 * blk_size[MAJOR][MINOR]
 *
 * if (!blk_size[MAJOR]) then no minor size checking is done.
 */
 /*
	 这个数组由主设备号和次设备号索引。它以KB为单位描述了每个设备的大小。
	 如果blk_size[major]是NULL，则不对这个设备的大小进行检查
	 （也就是说，核心可能要求数据传送通过end_of_device）
	 */
int * blk_size[MAX_BLKDEV] = { NULL, NULL, };

/*
 * blksize_size contains the size of all block-devices:
 *
 * blksize_size[MAJOR][MINOR]
 *
 * if (!blksize_size[MAJOR]) then 1024 bytes is assumed.
 */
 /*
	 被每个设备所使用的块的大小，以字节为单位。与上一个数组类似，
	 这个二维数组也是由主设备号和次设备号索引。如果blksize_size[major]
	 是一个空指针，那么便假设其块大小为BLOCK_SIZE（目前是1KB）。
	 块大小必须是2的幂，因为核心使用移位操作将偏移量转换为块号
*/
int * blksize_size[MAX_BLKDEV] = { NULL, NULL, };

/*
 * hardsect_size contains the size of the hardware sector of a device.
 *
 * hardsect_size[MAJOR][MINOR]
 *
 * if (!hardsect_size[MAJOR])
 *		then 512 bytes is assumed.
 * else
 *		sector_size is hardsect_size[MAJOR][MINOR]
 * This is currently set by some scsi device and read by the msdos fs driver
 * This might be a some uses later.
 */
int * hardsect_size[MAX_BLKDEV] = { NULL, NULL, };

/*
 * look for a free request in the first N entries.
 * NOTE: interrupts must be disabled on the way in, and will still
 *       be disabled on the way out.
 */
//从all_requests请求缓冲池中的前n项中请求一项空闲struct request结构
static inline struct request * get_request(int n, int dev)
{
	//这两个静态变量是为了提高系统查找效率，
	//还有一个重要的作用，等读到了make_request()函数就明白了
	static struct request *prev_found = NULL, *prev_limit = NULL;
	register struct request *req, *limit;

	if (n <= 0)
		panic("get_request(%d): impossible!\n", n);

	//limit指向all_requests数组中的第n项
	//从而将本次请求查找限定在数组的第0项到第n项之间
	limit = all_requests + n;
	//prev_limit保存上次请求的limit项，如果本次请求的limit和上次不同了
	//就需要重新设置prev_limit和prev_found的值了
	if (limit != prev_limit) {
		prev_limit = limit;
		//prev_found保存上次请求得到的项，这里将其设置为数组首地址
		//相当于置零了
		prev_found = all_requests;
	}
	req = prev_found;
	for (;;) {
		//循环内遍历每个request项的顺序是倒序的，即从第limit项
		//到第0项遍历
		req = ((req > all_requests) ? req : limit) - 1;
		//如果当前遍历的request项的dev小于0,说明其是未使用的，可以使用
		if (req->dev < 0)
			break;
		//如果在此limit范围内的数组项都遍历完了 则返回NULL
		if (req == prev_found)
			return NULL;
	}
	prev_found = req;
	//保存请求设备号
	req->dev = dev;
	return req;
}

/*
 * wait until a free request in the first N entries is available.
 * NOTE: interrupts must be disabled on the way in, and will still
 *       be disabled on the way out.
 */
//从all_requests请求缓冲池中的前n项中请求一项空闲struct request结构
//如果请求失败，则睡眠等待
static inline struct request * get_request_wait(int n, int dev)
{
	register struct request *req;

	while ((req = get_request(n, dev)) == NULL)
		sleep_on(&wait_for_request);
	return req;
}

/* RO fail safe mechanism */
/*
	内核中有个二维数组ro_bits[MAX_BLKDEV][8]
	每个设备在此数组中有标志位，通过iotcl可以将一个标志位设置为0/1
	is_read_only根据设备号就差这个数组中的标志位是否为1来检查读写允许情况
*/
static long ro_bits[MAX_BLKDEV][8];	//readonly_bits

//用于判断一个给定的设备是否是只读的
int is_read_only(int dev)
{
	int minor,major;

	major = MAJOR(dev);
	minor = MINOR(dev);
	if (major < 0 || major >= MAX_BLKDEV) return 0;
	return ro_bits[major][minor >> 5] & (1 << (minor & 31));
}

//用于设置一个制定的设备的只读属性
void set_device_ro(int dev,int flag)
{
	int minor,major;

	major = MAJOR(dev);
	minor = MINOR(dev);
	if (major < 0 || major >= MAX_BLKDEV) return;
	if (flag) ro_bits[major][minor >> 5] |= 1 << (minor & 31);
	else ro_bits[major][minor >> 5] &= ~(1 << (minor & 31));
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 */
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;
	short disk_index;

	switch (MAJOR(req->dev)) {
		case SCSI_DISK_MAJOR:	
			disk_index = (MINOR(req->dev) & 0x0070) >> 4;
			if (disk_index < 4)
				kstat.dk_drive[disk_index]++;
			break;
		case HD_MAJOR:
		case XT_DISK_MAJOR:	
			disk_index = (MINOR(req->dev) & 0x0040) >> 6;
			kstat.dk_drive[disk_index]++;
			break;
		case IDE1_MAJOR:	
			disk_index = ((MINOR(req->dev) & 0x0040) >> 6) + 2;
			kstat.dk_drive[disk_index]++;
		default:		break;
	}

	req->next = NULL;
	cli();
	//将缓冲区bh转移到干净页面的LRU队列中
	if (req->bh)
		mark_buffer_clean(req->bh);
	//如果此设备的请求队列为空，当此请求加入此设备的读写请求队列，
	//调用当前块设备的请求项操作函数进行读写操作
	if (!(tmp = dev->current_request)) {
		dev->current_request = req;
		(dev->request_fn)();
		sti();
		return;
	}
	//执行到这里，说明此设备的请求队列不为空，则利用电梯调度算法将此请求项插入队列
/*
	IN_ORDER(tmp，req)的作用类似于在比较tmp和req这两个请求。
	如果tmp请求“小于”req请求，则IN_ORDER为真。
	“小于”的意思：
	（1）读请求小于写请求。
	（2）若请求类型相同，低设备号小于高设备号。
	（3）若请求同一设备，低扇区号小于高扇区号。
*/
	for ( ; tmp->next ; tmp = tmp->next) {
		if ((IN_ORDER(tmp,req) ||
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	}
	//将请求链入 注意：这里并不改变第一个请求项的位置，也绝不能替换第一个请求项的位置，
	//原因请参见/linux/drivers/block/hd.c中hd_request()函数上面的注释
	req->next = tmp->next;
	tmp->next = req;

/* for SCSI devices, call request_fn unconditionally(无条件地) */
	//如果是scsi设备请求，则无条件立即执行？
	if (scsi_major(MAJOR(req->dev)))
		(dev->request_fn)();

	sti();
}

//将指定的缓冲区bh合并到已存在的请求项中或者单独形成一个请求项
static void make_request(int major,int rw, struct buffer_head * bh)
{
	unsigned int sector, count;
	struct request * req;
	int rw_ahead, max_req;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	rw_ahead = (rw == READA || rw == WRITEA);
	if (rw_ahead) {
		if (bh->b_lock)
			return;
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}
	if (rw!=READ && rw!=WRITE) {
		printk("Bad block dev command, must be R/W/RA/WA\n");
		return;
	}
	//将bh缓冲的大小转换成以扇区大小512B为单位的扇区数
	count = bh->b_size >> 9;
	//将bh的逻辑块号转换为扇区号
	sector = bh->b_blocknr * count;
	//blk_size数组中存放的是设备以KB为单位的大小
	if (blk_size[major])
		if (blk_size[major][MINOR(bh->b_dev)] < (sector + count)>>1) {
			bh->b_dirt = bh->b_uptodate = 0;
			bh->b_req = 0;
			return;
		}
	/* Uhhuh.. Nasty dead-lock possible here.. */
	if (bh->b_lock)
		return;
	/* Maybe the above fixes it, and maybe it doesn't boot. Life is interesting */
	lock_buffer(bh);
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
		unlock_buffer(bh);
		return;
	}

/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
	/*请求项数组中的后1/3只专供读请求使用，而前2/3读写请求都可以使用*/
	max_req = (rw == READ) ? NR_REQUEST : ((NR_REQUEST*2)/3);

/* big loop: look for a free request. */

repeat:
	cli();

/* The scsi disk drivers and the IDE driver completely remove the request
 * from the queue when they start processing an entry.  For this reason
 * it is safe to continue to add links to the top entry for those devices.
 */
	if ((   major == IDE0_MAJOR	/* same as HD_MAJOR */
	     || major == IDE1_MAJOR
	     || major == FLOPPY_MAJOR
	     || major == SCSI_DISK_MAJOR
	     || major == SCSI_CDROM_MAJOR)
	    && (req = blk_dev[major].current_request))
	{
#ifdef CONFIG_BLK_DEV_HD
	        if (major == HD_MAJOR || major == FLOPPY_MAJOR)
#else
		if (major == FLOPPY_MAJOR)
#endif CONFIG_BLK_DEV_HD
			req = req->next;	//为什么要跳过第一个请求项？？？
					//这是因为当请求队列为空时，要插入多个请求，为了防止立即执行请求，向当前请求队列插入了一个无效的请求

/*	
		博大精深的内核！！！上面关于要跳过（实际是为了避免替换掉当前请求项）第一个请求项的解释是不对的，
		原因请参见/linux/drivers/block/hd.c中hd_request()函数上面的注释
*/

		//这里是为了合并请求队列中读写命令一致的、并且读写扇区相邻的读写请求
		//如果找到了可以和当前bh合并的请求项，那么就可以不需要申请新的请求项
		//资源了，提高了系统的效率和节省了系统资源
		//我觉得循环中的两个if判断语句还可以做的更精湛，使其最多可以将之前两个不连续的请求项合二为一
		//也可以说是合三为一，其中本bh省下来的请求项也做为其一，然后还可以释放合并省下来的一向请求项资源
		/*这在《Linux Deviece Driver》中被称为“集簇”技术*/
		while (req) {
			if (req->dev == bh->b_dev &&
			    !req->sem &&
			    req->cmd == rw &&
			    req->sector + req->nr_sectors == sector &&
			    req->nr_sectors < 244)
			{
				//将此bh链入此请求项的bh链表中
				req->bhtail->b_reqnext = bh;
				req->bhtail = bh;
				//增加此请求项要读取的扇区数
				req->nr_sectors += count;
				//将此bh移到干净队列中
				mark_buffer_clean(bh);
				sti();
				return;
				//我的想法是在这里不要return，而是将此请求项和下一个请求项“合并”
			}

			if (req->dev == bh->b_dev &&
			    !req->sem &&
			    req->cmd == rw &&
			    req->sector - count == sector &&
			    req->nr_sectors < 244)
			{
			    	req->nr_sectors += count;
			    	bh->b_reqnext = req->bh;
			    	req->buffer = bh->b_data;
				//因为在这里是将此bh加入到此请求项的bh缓冲区头部
				//所以此bh成为了当此请求项得到执行的时候首先要执行
				//的“当前缓冲区”，所以需要更新此请求项的current_nr_sectors
				//其代表着当前缓冲区所对应的扇区数
			    	req->current_nr_sectors = count;
			    	req->sector = sector;
				mark_buffer_clean(bh);
			    	req->bh = bh;
			    	sti();
			    	return;
				//我的想法是在这里不要return，而是将此请求项和下一个请求项“合并”
			}    

			req = req->next;
		}
	}

	//执行到这里，说明本bh并没有合并到已经存在的请求项中，所以需要申请空闲的请求项结构，并加入队列

/* find an unused request. */
	//读到这里就可以清楚的明白get_request内两个静态变量的作用了
	req = get_request(max_req, bh->b_dev);

/* if no request available: if rw_ahead, forget it; otherwise try again. */
	if (! req) {
		if (rw_ahead) {
			sti();
			unlock_buffer(bh);
			return;
		}
		sleep_on(&wait_for_request);
		sti();
		goto repeat;
	}

/* we found a request. */
	sti();

/* fill up the request-info, and add it to the queue */
	//填充请求项信息
	req->cmd = rw;
	req->errors = 0;
	req->sector = sector;
	req->nr_sectors = count;
	req->current_nr_sectors = count;
	req->buffer = bh->b_data;
	req->sem = NULL;
	req->bh = bh;
	req->bhtail = bh;
	req->next = NULL;
	//将请求项插入设备请求队列
	add_request(major+blk_dev,req);
}

//读写一页4K
void ll_rw_page(int rw, int dev, int page, char * buffer)
{
	struct request * req;
	unsigned int major = MAJOR(dev);
	struct semaphore sem = MUTEX_LOCKED;

	if (major >= MAX_BLKDEV || !(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device %04x (%d)\n",dev,page*8);
		return;
	}
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W");
	if (rw == WRITE && is_read_only(dev)) {
		printk("Can't page to read-only device 0x%X\n",dev);
		return;
	}
	cli();
	req = get_request_wait(NR_REQUEST, dev);
	sti();
/* fill up the request-info, and add it to the queue */
	req->cmd = rw;
	req->errors = 0;
	//将页面号转换为扇区号，什么情况？
	req->sector = page<<3;
	req->nr_sectors = 8;
	req->current_nr_sectors = 8;
	req->buffer = buffer;
	req->sem = &sem;
	req->bh = NULL;
	req->next = NULL;
	add_request(major+blk_dev,req);
	down(&sem);
}

/* This function can be used to request a number of buffers from a block
   device. Currently the only restriction is that all buffers must belong to
   the same device */
/*
	ll_rw_block()主要功能是创建块设备读写请求项，并插入到对应块设备读写请求队列中
	（由i节点的主设备号决定）。如果当前请求项是该队列第一项，则立刻执行相应设备的
	请求函数，否则利用电梯算法插入到该队列中的合适位置。实际的读写操作是由设备的
	request_fn()函数来完成该设备队列的读/写请求操作。对于虚拟盘操作，该函数是
	do_rd_request()。对于硬盘操作，该函数是do_fd_request()。对于软盘操作，\
	该函数是do_fd_request()
	
	其参数的含义为：
	操作类型rw，其值可以是READ、WRITE、READA或者WRITEA。最后两种操作类型和前两种
	操作类型之间的区别在于，当没有可用的请求描述符时后两个函数不会阻塞。要传送的块数nr。
	一个bh数组，有nr个指针，指向说明块的缓冲区首部（这些块的大小必须相同，而且必须处于同一个块设备）。 
*/
void ll_rw_block(int rw, int nr, struct buffer_head * bh[])
{
	unsigned int major;
	struct request plug;
	int plugged;
	int correct_size;
	struct blk_dev_struct * dev;
	int i;

	/* Make sure that the first block contains something reasonable */
	while (!*bh) {
		bh++;
		if (--nr <= 0)
			return;
	};

	dev = NULL;
	if ((major = MAJOR(bh[0]->b_dev)) < MAX_BLKDEV)
		dev = blk_dev + major;
	if (!dev || !dev->request_fn) {
		printk(
	"ll_rw_block: Trying to read nonexistent block-device %04lX (%ld)\n",
		       (unsigned long) bh[0]->b_dev, bh[0]->b_blocknr);
		goto sorry;
	}

	/* Determine correct block size for this device.  */
	//选择此设备的块大小，默认是BLOCK_SIZE，即1024B（1KB）
	correct_size = BLOCK_SIZE;
	if (blksize_size[major]) {
		i = blksize_size[major][MINOR(bh[0]->b_dev)];
		if (i)
			correct_size = i;
	}

	/* Verify requested block sizes.  */
	for (i = 0; i < nr; i++) {
		if (bh[i] && bh[i]->b_size != correct_size) {
			printk(
			"ll_rw_block: only %d-char blocks implemented (%lu)\n",
			       correct_size, bh[i]->b_size);
			goto sorry;
		}
	}

	if ((rw == WRITE || rw == WRITEA) && is_read_only(bh[0]->b_dev)) {
		printk("Can't write to read-only device 0x%X\n",bh[0]->b_dev);
		goto sorry;
	}

	/* If there are no pending requests for this device, then we insert
	   a dummy request for that device.  This will prevent the request
	   from starting until we have shoved all of the blocks into the
	   queue, and then we let it rip.  */
	//shoved:v. 	推，猛推，乱推( shove的过去式和过去分词 );乱放;随便放;胡乱丢
	//rip:扯破，撕坏
	plugged = 0;
	cli();
	//如果设备当前请求队列为空并且要读写的块数大于1,则需要在队列中插入一个
	//dummy request以阻止当插入第一个请求项时就立即执行
	if (!dev->current_request && nr > 1) {
		dev->current_request = &plug;
		plug.dev = -1;
		plug.next = NULL;
		plugged = 1;
	}
	sti();
	for (i = 0; i < nr; i++) {
		if (bh[i]) {
			bh[i]->b_req = 1;
			make_request(major, rw, bh[i]);
			if (rw == READ || rw == READA)
				kstat.pgpgin++;
			else
				kstat.pgpgout++;
		}
	}
	if (plugged) {
		cli();
		dev->current_request = plug.next;
		(dev->request_fn)();
		sti();
	}
	return;

      sorry:
	for (i = 0; i < nr; i++) {
		if (bh[i])
			bh[i]->b_dirt = bh[i]->b_uptodate = 0;
	}
	return;
}

void ll_rw_swap_file(int rw, int dev, unsigned int *b, int nb, char *buf)
{
	int i;
	int buffersize;
	struct request * req;
	unsigned int major = MAJOR(dev);
	struct semaphore sem = MUTEX_LOCKED;

	if (major >= MAX_BLKDEV || !(blk_dev[major].request_fn)) {
		printk("ll_rw_swap_file: trying to swap nonexistent block-device\n");
		return;
	}

	if (rw!=READ && rw!=WRITE) {
		printk("ll_rw_swap: bad block dev command, must be R/W");
		return;
	}
	if (rw == WRITE && is_read_only(dev)) {
		printk("Can't swap to read-only device 0x%X\n",dev);
		return;
	}
	
	buffersize = PAGE_SIZE / nb;

	for (i=0; i<nb; i++, buf += buffersize)
	{
		cli();
		req = get_request_wait(NR_REQUEST, dev);
		sti();
		req->cmd = rw;
		req->errors = 0;
		req->sector = (b[i] * buffersize) >> 9;
		req->nr_sectors = buffersize >> 9;
		req->current_nr_sectors = buffersize >> 9;
		req->buffer = buf;
		req->sem = &sem;
		req->bh = NULL;
		req->next = NULL;
		add_request(major+blk_dev,req);
		down(&sem);
	}
}

long blk_dev_init(long mem_start, long mem_end)
{
	struct request * req;

	req = all_requests + NR_REQUEST;
	while (--req >= all_requests) {
		req->dev = -1;
		req->next = NULL;
	}
	memset(ro_bits,0,sizeof(ro_bits));
#ifdef CONFIG_BLK_DEV_HD
	mem_start = hd_init(mem_start,mem_end);
#endif
#ifdef CONFIG_BLK_DEV_IDE
	mem_start = ide_init(mem_start,mem_end);
#endif
#ifdef CONFIG_BLK_DEV_XD
	mem_start = xd_init(mem_start,mem_end);
#endif
#ifdef CONFIG_CDU31A
	mem_start = cdu31a_init(mem_start,mem_end);
#endif
#ifdef CONFIG_CDU535
	mem_start = sony535_init(mem_start,mem_end);
#endif
#ifdef CONFIG_MCD
	mem_start = mcd_init(mem_start,mem_end);
#endif
#ifdef CONFIG_AZTCD
        mem_start = aztcd_init(mem_start,mem_end);
#endif
#ifdef CONFIG_BLK_DEV_FD
	floppy_init();
#else
	outb_p(0xc, 0x3f2);
#endif
#ifdef CONFIG_SBPCD
	mem_start = sbpcd_init(mem_start, mem_end);
#endif CONFIG_SBPCD
	if (ramdisk_size)
		mem_start += rd_init(mem_start, ramdisk_size*1024);
	return mem_start;
}
