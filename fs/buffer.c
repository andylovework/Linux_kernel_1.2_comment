/*
 *  linux/fs/buffer.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */
 
/*
 * 用于对高数缓冲区进行操作和管理。高速缓冲区位于内核代码和主内存区之间，如下图，高速缓冲区在块设备-与
 * 内核其他程序之间起一个桥梁作用
 *   内核模块   高        速        缓        冲 高速缓冲        主 内 存
 * |         |        |显存和BIOS ROM|         |       |                         |
 * 0         end      640kb         1M        4M     5M                        16M
*/

/*
 * 'buffer.c' 用于实现缓冲区高速缓存功能，通过不让终端处理过程改变缓冲区，而是让调用者来执行
 * 避免克竞争条件(当然除改变数据以外)。注意！由于中断可以唤醒一个调用者，因此就需要开关中断指令(cli-sti)
 * 序列来检测用于调用为而睡眠。但需要非常快 
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */
 
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/errno.h>
#include <linux/malloc.h>

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/io.h>

/*
	不同逻辑块设备的数据块（并非扇区，扇区是物理块设备组织数据的单位，通常是512字节，
	数据块的大小必须是扇区大小的整数倍）大小通常都不同。Linux中允许有NR_SIZES种大小的块，
	分别为：512、1024、2048、4096、8192、16384和32768字节。由于缓冲区与块是对应的，
	因此缓冲区的大小也相应地有7种。但是由于一个块的大小不能超过物理页帧的大小，
	因此在i386体系结构中实际上只能使用前4种大小的块
*/

#define NR_SIZES 4

/*
	为了更方便地根据块大小找到相应的空闲缓冲区头部对象链表，Linux定义了
	数组buffersize_index和宏BUFSIZE_INDEX
*/

static char buffersize_index[9] = {-1,  0,  1, -1,  2, -1, -1, -1, 3};	//-1表示无效，否则为bufferindex_size的下标
static short int bufferindex_size[NR_SIZES] = {512, 1024, 2048, 4096};

/*
可以看出，只有当块大小x属于集合〔512,1024,2048,…,32768〕时，它在数组buffersize_index中的索引值才有意义。
宏BUFSIZE_INDEX是用来根据大小x索引数组free_list的，其用法通常为：free_list〔BUDSIZE_INDEX（x）〕。
*/

#define BUFSIZE_INDEX(X) ((int) buffersize_index[(X)>>9])
#define MAX_BUF_PER_PAGE (PAGE_SIZE / 512)

static int grow_buffers(int pri, int size);
static int shrink_specific_buffers(unsigned int priority, int size);
static int maybe_shrink_lav_buffers(int);

/*
	对于包含了有效数据的缓冲区，用一个哈希表来管理，用hash_table来指向这个哈希表。
	哈希索引值由数据块号以及其所在的设备标识号计算（散列）得到。所以在buffer_head
	这个结构中有一些用于哈希表管理的域。使用哈希表可以迅速地查找到所要寻找的数据块所在的缓冲区。 
*/

static int nr_hash = 0;  /* Size of hash table */
static struct buffer_head ** hash_table;
struct buffer_head ** buffer_pages; //类似于mem_map swap_cache的数组

/*
	对于每一种类型的未使用的有效缓冲区，系统还使用一个LRU（最近最少使用）双链表管理，
	即lru-list链。由于共有三种类型的缓冲区，所以有三个这样的LRU链表。当需要访问某个数据块时，系统采取如下算法： 
	首先，根据数据块号和所在设备号在块高速缓存中查找，如果找到，则将它的b-count域加1，
	因为这个域正是反映了当前使用这个缓冲区的进程数。如果这个缓冲区同时又处于某个LRU链中，则将它从LRU链中解开。 
	如果数据块还没有调入缓冲区，则系统必须进行磁盘I/O操作，将数据块调入块高速缓存，同时将空缓冲区分配一个给它。
	如果块高速缓存已满（即没有空缓冲区可供分配），则从某个LRU链首取下一个，先看是否置了“脏”位，如已置，
	则将它的内容写回磁盘。然后清空内容，将它分配给新的数据块。 
	在缓冲区使用完了后，将它的b_count域减1，如果b_count变为0，则将它放在某个LRU链尾，表示该缓冲区已可以重新利用。 
*/

//NR_LIST定义在Fs.h文件中，其中并定义了NR_LIST种类型的未使用的有效缓冲区，参见Fs.h
// Linux在fs.h头文件中为lru_list〔〕数组中的每一元素（也即链表表头）定义了索引宏
//比如：#define BUF_CLEAN 0->BUF_CLEAN链表：这条链表集中存放干净缓冲区（没有设置BH_Dirty标志）的buffer_head对象。
//注意！该链表中的缓冲区为必是最新的;#define BUF_DIRTY 2->BUF_DIRTY链表：这条链表主要集中存放脏缓冲区的缓冲区首部，
//但这些缓冲区还未被选中要把其中的内容回写到磁盘，也即BH_Dirty被置位，但BH_Lock未被置位。 
//#define BUF_LOCKED 1  BUF_LOCKED链表：这个链表主要存放已经被选中要把其中的内容回写到磁盘的脏缓冲区的buffer_head对象，
//也即：BH_Lock已经被置位，而BH_Dirty标志已经被清除

/*
	 bcache中的缓冲区头部对象链表 
	一个缓冲区头部对象buffer_head总是处于以下四种状态之一： 
	1. 未使用（unused）状态：该对象是可用的，但是其b_data指针为NULL，也即这个缓冲区头部没有和一个缓冲区相关联。 
	2. 空闲（free）状态：其b_data指针指向一个空闲状态下的缓冲区（也即该缓冲区没有具体对应块设备中哪个数据块）；而b_dev域值为B_FREE（值为0xffff）。 
	3. 正在使用（inuse）状态：其b_data指针指向一个有效的、正在使用中的缓冲区，而b_dev域则指明了相应的块设备标识符，b_blocknr域则指明了缓冲区所对应的块号。 
	4. 异步（async）状态：其b_data域指向一个用来实现page I/O操作的临时缓冲区。 
	为了有效地管理处于上述这些不同状态下的缓冲区头部对象，bcache机制采用了各种链表来组织这些对象（这一点，bcache机制与VFS的其他cache机制是相同的）： 
	1. 哈希链表：所有buffer_head对象都通过其b_next与b_pprev两个指针域链入哈希链表中，从而可以加快对buffer_head对象的查找（lookup）。 
	2. 最近最少使用链表lru_list：每个处在inuse状态下的buffer_head对象都通过b_next_free和b_prev_free这两个指针链入某一个lru_list链表中。 
	3. 空闲链表free_list：每一个处于free状态下的buffer_head对象都根据它所关联的空闲缓冲区的大小链入某个free_list链表中（也是通过b_next_free和b_prev_free这两个指针）。 
	4. 未使用链表unused_list：所有处于unused状态下的buffer_head对象都通过指针域b_next_free和b_prev_free链入unused_list链表中。 
	5. inode对象的脏缓冲区链表i_dirty_buffers：如果一个脏缓冲区有相关联的inode对象的话，那么他就通过其b_inode_buffers指针域链入其所属的inode对象的i_dirty_buffers链表中。 
*/

	/*NOTE：个人感觉 NR_LIST改为NR_TYPE更为贴切*/
static struct buffer_head * lru_list[NR_LIST] = {NULL, };

/*
	为了配合以上这些操作，以及其它一些多块高速缓存的操作，系统另外使用了几个链表，主要是： 
	·对于每一种大小的空闲缓冲区，系统使用一个链表管理，即free_list链。 
	·对于空缓冲区，系统使用一个unused_list链管理。
*/

//对于每一种大小的空闲缓冲区，系统使用一个链表管理，即free_list链
//而所有NR_SIZES条空闲缓冲区的头部对象链表一起组成一个数组
static struct buffer_head * free_list[NR_SIZES] = {NULL, };
//对于空缓冲区，系统使用一个unused_list链管理
static struct buffer_head * unused_list = NULL;
static struct wait_queue * buffer_wait = NULL;

/*
	nr_buffers_type〔〕表示这4条链表中每条链表的元素个数，
	数组size_buffers_type（nr_buffers_st）则表示这4条链表中每条链表中的所有缓冲区的大小总和
	可参考insert_into_queues函数理解
	举例来讲就是，nr_buffers_st[0][0]代表大小为512字节的缓冲区中，类型为0（BUF_CLEAN）的缓冲区个数
*/
int nr_buffers = 0;		//所有的buffer个数
int nr_buffers_type[NR_LIST] = {0,};		//每种类型的buffer链表中buffer的个数
int nr_buffers_size[NR_SIZES] = {0,};		//每种大小的buffer的链表中buffer的个数
int nr_buffers_st[NR_SIZES][NR_LIST] = {{0,},};	//每种大小的链表中每种类型的buffer个数
int buffer_usage[NR_SIZES] = {0,};  /* Usage counts used to determine load average */
int buffers_lav[NR_SIZES] = {0,};  /* Load average of buffer usage */
int nr_free[NR_SIZES] = {0,};	//空闲缓冲块数数组，针对每种大小的缓冲区，各占数组中相应的一项，内容代表此大小的空闲缓冲区个数
int buffermem = 0;	//表示缓冲区的总内存大小
int nr_buffer_heads = 0;	//buffer_head个数
extern int *blksize_size[];

/* Here is the parameter block for the bdflush process. */
static void wakeup_bdflush(int);

#define N_PARAM 9
#define LAV

static union bdflush_param{
	struct {
		int nfract;  /* Percentage of buffer cache dirty to 
				activate bdflush */
		int ndirty;  /* Maximum number of dirty blocks to write out per
				wake-cycle */
		int nrefill; /* Number of clean buffers to try and obtain
				each time we call refill */
		int nref_dirt; /* Dirty buffer threshold for activating bdflush
				  when trying to refill buffers. */
		int clu_nfract;  /* Percentage of buffer cache to scan to 
				    search for free clusters */
		int age_buffer;  /* Time for normal buffer to age before 
				    we flush it */
		int age_super;  /* Time for superblock to age before we 
				   flush it */
		int lav_const;  /* Constant used for load average (time
				   constant */
		int lav_ratio;  /* Used to determine how low a lav for a
				   particular size can go before we start to
				   trim back the buffers */
	} b_un;
	unsigned int data[N_PARAM];
} bdf_prm = {{25, 500, 64, 256, 15, 3000, 500, 1884, 2}};

/* The lav constant is set for 1 minute, as long as the update process runs
   every 5 seconds.  If you change the frequency of update, the time
   constant will also change. */

/*
	Linux中，用bdflush守护进程完成对块高速缓存的一般管理。bdflush守护进程是一个简单的内核线程，
	在系统启动时运行，它在系统中注册的进程名称为 kflushd，你可以使用ps命令看到此系统进程。它的
	一个作用是监视块高速缓存中的“脏”缓冲区，在分配或丢弃缓冲区时，将对“脏”缓冲区数目作一个统计。
	通常情况下，该进程处于休眠状态，当块高速缓存中“脏”缓冲区的数目达到一定的比例，默认是60%，
	该进程将被唤醒。但是，如果系统急需，则在任何时刻都可能唤醒这个进程。使用update命令可以看到和改变这个数值。 

	# update -d 

	当有数据写入缓冲区使之变成“脏”时，所有的“脏”缓冲区被连接到一个BUF_DIRTY_LRU链表中，bdflush会将适当数目
	的缓冲区中的数据块写到磁盘上。这个数值的缺省值为500，可以用update命令改变这个值。 

*/

/* These are the min and max parameter values that we will allow to be assigned */
static int bdflush_min[N_PARAM] = {  0,  10,    5,   25,  0,   100,   100, 1, 1};
static int bdflush_max[N_PARAM] = {100,5000, 2000, 2000,100, 60000, 60000, 2047, 5};

/*
 * Rewrote the wait-routines to use the "new" wait-queue functionality,
 * and getting rid of the cli-sti pairs. The wait-queue routines still
 * need cli-sti, but now it's just a couple of 386 instructions or so.
 *
 * Note that the real wait_on_buffer() is an inline function that checks
 * if 'b_wait' is set before calling this, so that the queues aren't set
 * up unnecessarily.
 */
void __wait_on_buffer(struct buffer_head * bh)
{
	struct wait_queue wait = { current, NULL };

	bh->b_count++;
	add_wait_queue(&bh->b_wait, &wait);
repeat:
	current->state = TASK_UNINTERRUPTIBLE;
	if (bh->b_lock) {
		schedule();
		goto repeat;
	}
	remove_wait_queue(&bh->b_wait, &wait);
	bh->b_count--;
	current->state = TASK_RUNNING;
}

/* Call sync_buffers with wait!=0 to ensure that the call does not
   return until all buffer writes have completed.  Sync() may return
   before the writes have finished; fsync() may not. */


/* Godamity-damn.  Some buffers (bitmaps for filesystems)
   spontaneously dirty themselves without ever brelse being called.
   We will ultimately want to put these in a separate list, but for
   now we search all of the lists for dirty buffers */

//将属于某个特定块设备的所有脏缓冲区真正地回写到块设备中
/*
	参数dev指定逻辑块设备的设备标识符，参数wait指定是否等待所有脏缓冲区的回写操作完成后函数才返回。 
	对于wait=0的情况，sync_buffers()函数仅仅只是扫描BUF_DIRTY链表，并对其中的脏缓冲区安排回写操作即可
	（通过调用块设备驱动程序的ll_rw_block()函数）。 对于wait非0的情况，处理就比较复杂些。Sync_buffers()
	函数在一个do{}while循环中分三次扫描处理BUF_DIRTY链表和BUF_LOCKED链表：①第一遍扫描，仅仅对BUF_DIRTY
	链表中的Dirty且unlocked的缓冲区通过ll_rw_block()函数安排回写操作；②第二编循环中主要调用wait_on_buffer()
	函数等待第一遍所安排的回写操作真正完成。由于在等待过程中，可能会有新的脏缓冲区插入到BUF_DIRTY链表中，
	因此在第二编循环中，对BUF_DIRTY链表和BUF_LOCKED链表的扫描每次总是从链表的表头开始，如果扫描的过程中碰到Dirty缓冲区，
	那么也要通过ll_rw_block()函数对其安排回写操作。在第二编循环结束时，BUF_DIRTY链表中将不再有任何Dirty缓冲区。
	③第三便循环时这仅仅是为了等待第二遍所安排的回写操作结束
*/
//dev=0则同步所有设备 依据wait的值决定循环次数（1or3）
static int sync_buffers(dev_t dev, int wait)
{
	int i, retry, pass = 0, err = 0;
	int nlist, ncount;
	struct buffer_head * bh, *next;

	/* One pass for no-wait, three for wait:
	   0) write out all dirty, unlocked buffers;
	   1) write out all dirty buffers, waiting if locked;
	   2) wait for completion by waiting for all buffers to unlock. */
 repeat:
	retry = 0;
 repeat2:
	ncount = 0;	//用来记录是脏的，但是却没有在脏链表中的buffer个数
	/* We search all lists as a failsafe mechanism, not because we expect
	   there to be dirty buffers on any of the other lists. */
	for(nlist = 0; nlist < NR_LIST; nlist++)
	 {
	 repeat1:
		 bh = lru_list[nlist];
		 if(!bh) continue;
		 //由于是一个双向循环链表，因此for循环将链表扫描两次(why? I don’t know^_^ If you know, please tell me.)
		 for (i = nr_buffers_type[nlist]*2 ; i-- > 0 ; bh = next) {
			 if(bh->b_list != nlist) goto repeat1;
			 next = bh->b_next_free;
			 //链表为空 终止扫描过程 如果为NULL，则终止扫描过程。因为每一个被安排回写的脏缓冲区都会被移到BUF_LOCKED链表中，
			 //从而使BUF_DIRTY链表中的元素会越来越少。因此这里在开始处理之前有必要进行一下判断
			 if(!lru_list[nlist]) break;	
			 //如果参数dev非0，则进一步判断当前被扫描的缓冲区是否属于指定的块设备。
			 //如果不是，则扫描量表中的下一个元素。当dev=0时，sync_buffers()函数同步所有脏缓冲区（不论它是属于哪个块设备）
			 if (dev && bh->b_dev != dev)
				  continue;
			//判断被扫描的缓冲区是否已经被加锁（是否以被选中去做回写操作）。如果是，则进一步判断wait和pass的值。
			//如果wait=0或pass=0（即第一遍do{}while循环），则不等待该缓冲区的回写操作完成，而是继续扫描链表中的
			//下一个元素。否则（wait!=0且pass!=0）就调用wait_on_buffer()函数等待该缓冲区被解锁，然后执行goto repeat语句，
			//重新从链表的开头开始扫描
			 if (bh->b_lock)
			  {
				  /* Buffer is locked; skip it unless wait is
				     requested AND pass > 0. */
				  if (!wait || !pass) {
					  retry = 1;
					  continue;
				  }
				  wait_on_buffer (bh);
				  goto repeat2;
			  }
			 /* If an unlocked buffer is not uptodate, there has
			     been an IO error. Skip it. */
			 if (wait && bh->b_req && !bh->b_lock &&
			     !bh->b_dirt && !bh->b_uptodate) {
				  err = 1;
				  printk("Weird - unlocked, clean and not uptodate buffer on list %d %x %lu\n", nlist, bh->b_dev, bh->b_blocknr);
				  continue;
			  }
			 /* Don't write clean buffers.  Don't write ANY buffers
			    on the third pass. */
			 if (!bh->b_dirt || pass>=2)
				  continue;
			 /* don't bother about locked buffers */
			 if (bh->b_lock)
				 continue;
			 bh->b_count++;
			 bh->b_flushtime = 0;
			 ll_rw_block(WRITE, 1, &bh);

			 if(nlist != BUF_DIRTY) { 
				 printk("[%d %x %ld] ", nlist, bh->b_dev, bh->b_blocknr);
				 ncount++;
			 };
			 bh->b_count--;
			 retry = 1;
		 }
	 }
	if (ncount) printk("sys_sync: %d dirty buffers not on dirty list\n", ncount);
	
	/* If we are waiting for the sync to succeed, and if any dirty
	   blocks were written, then repeat; on the second pass, only
	   wait for buffers being written (do not pass to write any
	   more buffers on the second pass). */
	if (wait && retry && ++pass<=2)
		 goto repeat;
	return err;
}

void sync_dev(dev_t dev)
{
	sync_buffers(dev, 0);
	sync_supers(dev);
	sync_inodes(dev);
	sync_buffers(dev, 0);
}

int fsync_dev(dev_t dev)
{
	sync_buffers(dev, 0);
	sync_supers(dev);
	sync_inodes(dev);
	return sync_buffers(dev, 1);
}

asmlinkage int sys_sync(void)
{
	sync_dev(0);
	return 0;
}

int file_fsync (struct inode *inode, struct file *filp)
{
	return fsync_dev(inode->i_dev);
}

asmlinkage int sys_fsync(unsigned int fd)
{
	struct file * file;
	struct inode * inode;

	if (fd>=NR_OPEN || !(file=current->files->fd[fd]) || !(inode=file->f_inode))
		return -EBADF;
	if (!file->f_op || !file->f_op->fsync)
		return -EINVAL;
	if (file->f_op->fsync(inode,file))
		return -EIO;
	return 0;
}

void invalidate_buffers(dev_t dev)
{
	int i;
	int nlist;
	struct buffer_head * bh;

	for(nlist = 0; nlist < NR_LIST; nlist++) {
		bh = lru_list[nlist];
		for (i = nr_buffers_type[nlist]*2 ; --i > 0 ; bh = bh->b_next_free) {
			if (bh->b_dev != dev)
				continue;
			wait_on_buffer(bh);
			if (bh->b_dev != dev)
				continue;
			if (bh->b_count)
				continue;
			bh->b_flushtime = bh->b_uptodate = 
				bh->b_dirt = bh->b_req = 0;
		}
	}
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%nr_hash)
#define hash(dev,block) hash_table[_hashfn(dev,block)]

static inline void remove_from_hash_queue(struct buffer_head * bh)
{
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
	bh->b_next = bh->b_prev = NULL;
}

//将一个指定的bh对象从其当前所属的lru_list〔blist〕链表中删除
static inline void remove_from_lru_list(struct buffer_head * bh)
{
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("VFS: LRU block list corrupted");
	if (bh->b_dev == 0xffff)  //若此bh为未使用的，说明其不应该出现在lru链表中 内核出错 死机
		panic("LRU list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;

	if (lru_list[bh->b_list] == bh)
		 lru_list[bh->b_list] = bh->b_next_free;
	if(lru_list[bh->b_list] == bh)
		 lru_list[bh->b_list] = NULL;
	bh->b_next_free = bh->b_prev_free = NULL;
}

//从free_list〔index〕摘除指定的bh对象
static inline void remove_from_free_list(struct buffer_head * bh)
{
    int isize = BUFSIZE_INDEX(bh->b_size);	//得到此bh在free_list数组中相应的下标索引
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("VFS: Free block list corrupted");
	if(bh->b_dev != 0xffff) panic("Free list corrupted");	//如果此bh不是被标记为0xffff，则说明free_list有错，死机
	if(!free_list[isize])	//要释放的bh所在的free_list为空 说明内核出错 死机
		 panic("Free list empty");
	nr_free[isize]--;	//将相应大小的缓冲区个数减一
	if(bh->b_next_free == bh)	//说明链表中就此一项
		 free_list[isize] = NULL;
	else {
		bh->b_prev_free->b_next_free = bh->b_next_free;
		bh->b_next_free->b_prev_free = bh->b_prev_free;
		if (free_list[isize] == bh)
			 free_list[isize] = bh->b_next_free;
	};
	//将要释放的bh指针置空
	bh->b_next_free = bh->b_prev_free = NULL;
}

/*
Linux在buffer.c文件中还封装了两个函数insert_into_queues()和remove_from_queues()，
用于实现对哈希链表和lru_list链表的同时插入和删除操作
*/
static inline void remove_from_queues(struct buffer_head * bh)
{
        if(bh->b_dev == 0xffff) {
			remove_from_free_list(bh); /* Free list entries should not be
					      in the hash queue */
		return;
	};
	nr_buffers_type[bh->b_list]--;
	nr_buffers_st[BUFSIZE_INDEX(bh->b_size)][bh->b_list]--;
	remove_from_hash_queue(bh);
	remove_from_lru_list(bh);
}

static inline void put_last_lru(struct buffer_head * bh)
{
	if (!bh)
		return;
	if (bh == lru_list[bh->b_list]) {
		lru_list[bh->b_list] = bh->b_next_free;
		return;
	}
	if(bh->b_dev == 0xffff) panic("Wrong block for lru list");
	remove_from_lru_list(bh);
/* add to back of free list */

	if(!lru_list[bh->b_list]) {
		lru_list[bh->b_list] = bh;
		lru_list[bh->b_list]->b_prev_free = bh;
	};

	bh->b_next_free = lru_list[bh->b_list];
	bh->b_prev_free = lru_list[bh->b_list]->b_prev_free;
	lru_list[bh->b_list]->b_prev_free->b_next_free = bh;
	lru_list[bh->b_list]->b_prev_free = bh;
}

//将一个刚转变为free状态的缓冲区的bh对象放入它应该所属的free_list〔index〕链表中
static inline void put_last_free(struct buffer_head * bh)
{
    int isize;
	if (!bh)
		return;

	isize = BUFSIZE_INDEX(bh->b_size);	//取得此缓冲区应在的free_list数组索引下标
	bh->b_dev = 0xffff;  /* So it is obvious we are on the free list *//* Flag as unused */
/* add to back of free list */

	//若此链表为空
	if(!free_list[isize]) {
		free_list[isize] = bh;
		bh->b_prev_free = bh;
	};

	nr_free[isize]++;	//此类型的缓冲区个数加一
	bh->b_next_free = free_list[isize];
	bh->b_prev_free = free_list[isize]->b_prev_free;
	free_list[isize]->b_prev_free->b_next_free = bh;
	free_list[isize]->b_prev_free = bh;
}

static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */

	//如果此bh未被使用 则加入到free_list
    if(bh->b_dev == 0xffff) {
		put_last_free(bh);
		return;
	};
	//若此链表为空
	if(!lru_list[bh->b_list]) {
		lru_list[bh->b_list] = bh;
		bh->b_prev_free = bh;
	};
	if (bh->b_next_free) panic("VFS: buffer LRU pointers corrupted");
	bh->b_next_free = lru_list[bh->b_list];
	bh->b_prev_free = lru_list[bh->b_list]->b_prev_free;
	lru_list[bh->b_list]->b_prev_free->b_next_free = bh;
	lru_list[bh->b_list]->b_prev_free = bh;
	/*
	将链表元素个数值nr_buffers_type〔blist〕加1，
	同时增加这个链表的缓冲区总大小值size_buffers_type〔blist〕
	*/
	nr_buffers_type[bh->b_list]++;	//将此种类型的bh数加一
	nr_buffers_st[BUFSIZE_INDEX(bh->b_size)][bh->b_list]++;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	if (bh->b_next)
		bh->b_next->b_prev = bh;
}

static struct buffer_head * find_buffer(dev_t dev, int block, int size)
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			if (tmp->b_size == size)
				return tmp;
			else {
				printk("VFS: Wrong blocksize on device %d/%d\n",
							MAJOR(dev), MINOR(dev));
				return NULL;
			}
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are reading them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
 /*
 在哈希链表中查找一个特定的缓冲区首部函数get_hash_table()在哈希链表中查找是否存在给定条件
 （dev,block,size）的buffer_head对象，如果存在则增加该buffer_head对象的引用计数值
 */
 
struct buffer_head * get_hash_table(dev_t dev, int block, int size)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block,size)))
			return NULL;
		bh->b_count++;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block && bh->b_size == size)
			return bh;
		bh->b_count--;
	}
}

void set_blocksize(dev_t dev, int size)
{
	int i, nlist;
	struct buffer_head * bh, *bhnext;

	//blksize_size数组存储的是各设备以字节为单位的块大小
	if (!blksize_size[MAJOR(dev)])
		return;

	//块大小不能超过物理页大小，但又必须是磁盘扇区大小的整数倍
	switch(size) {
		default: panic("Invalid blocksize passed to set_blocksize");
		case 512: case 1024: case 2048: case 4096:;
	}

	//如果该设备之前未设置块大小(为0，表面之前设备未使用？)，并且要设置的块大小为默认大小：BLOCK_SIZE
	//则将该设备块大小设为size后return
	if (blksize_size[MAJOR(dev)][MINOR(dev)] == 0 && size == BLOCK_SIZE) {
		blksize_size[MAJOR(dev)][MINOR(dev)] = size;
		return;
	}
	//如果要设置的块大小size值等于该设备之前的块大小，则直接return
	if (blksize_size[MAJOR(dev)][MINOR(dev)] == size)
		return;
	//执行到这里，则需要刷新缓冲了，因为要改变设备块大小信息了
	sync_buffers(dev, 2);
	blksize_size[MAJOR(dev)][MINOR(dev)] = size;

  /* We need to be quite careful how we do this - we are moving entries
     around on the free list, and we can get in a loop if we are not careful.*/
	//需要将该设备所对应的缓冲块从链表中remove出去
	for(nlist = 0; nlist < NR_LIST; nlist++) {
		bh = lru_list[nlist];
		for (i = nr_buffers_type[nlist]*2 ; --i > 0 ; bh = bhnext) {
			if(!bh) break;
			bhnext = bh->b_next_free; 
			if (bh->b_dev != dev)
				 continue;
			if (bh->b_size == size)
				 continue;
			
			wait_on_buffer(bh);
			//bh->b_size != size因为上面等待缓冲区解锁可能导致睡眠，在睡眠期间
			//缓冲区链表中可能加入该设备改变size之后的缓冲块，所以需要判断一下
			if (bh->b_dev == dev && bh->b_size != size) {
				bh->b_uptodate = bh->b_dirt = bh->b_req =
					 bh->b_flushtime = 0;
			};
			remove_from_hash_queue(bh);
		}
	}
}

#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)

//在系统运行时，如果某个空闲缓冲区链表free_list〔i〕为空，
//则需要从Buddy系统中申请分配额外的缓冲区页，并在其中创建相应大小的新空闲缓冲区。
//调用grow_buffer()函数来实际进行新缓冲区的分配工作
/*
	（1）首先，判断参数size是否为512的倍数，是否大于PAGE_SIZE。 
	（2）然后，调用Buddy系统的alloc_page()宏分配一个新的物理页帧。如果分配失败，则跳转到out部分，直接返回（返回值为0）。
	如果分配成功，则调用LockPage()宏（Mm.h）对该物理页帧进行加锁（即设置page->flags的PG_locked标志位）。 
	（3）然后调用create_buffers()函数在所分配的物理页帧中创建空闲缓冲区，该函数返回该物理页帧中的第一个缓冲区
	(首地址的页内偏移为0的那个缓冲区)的buffer_head对象指针。每一个buffer_head对象中的b_this_page指向该物理页帧中
	的下一个缓冲区，但是最后一个缓冲区的buffer_head对象的b_this_page指针为NULL。 
	（4）如果create_buffers()函数返回NULL，则说明创建缓冲区失败，失败的原因是不能从buffer_head对象的缓存
	（包括unused_list链表和bh_cachep SLAB缓存）中得到一个未使用的buffer_head对象。于是跳转到no_buffer_head部分，该部分做两件事：
	用UnlockPage宏对所分配的缓冲区进行解锁；
	调用page_cache_release()宏（实际上就是Buddy系统的__free_page宏）释放所分配的缓冲区。 
	（5）如果create_buffers()函数返回非NULL指针。则接下来的while循环将把所创建的空闲缓冲区的buffer_head对象插入到相对
	应的free_list〔I〕链表的首部。然后，修改缓冲区中的最后一个缓冲区的b_this_page指针，使其指向第一个缓冲区的buffer_head对象；
	同时修改free_list〔I〕链表的表头指针。 
	（6）最后，将page->buffers指针指向第一个缓冲区的buffer_head对象，并对缓冲区页进行解锁，增加变量buffermem_pages的值（加1），
	然后返回1表示grow_buffers函数执行成功。 
	函数create_buffers()在指定的空闲缓冲区页内常见特定大小的缓冲区。NOTE! 如果参数async＝1的话，则表明函数是在为异步页
	I／O创建空闲缓冲区，此时该函数必须总是执行成功。
*/
void refill_freelist(int size)
{
	struct buffer_head * bh, * tmp;
	struct buffer_head * candidate[NR_LIST];
	unsigned int best_time, winner;
    int isize = BUFSIZE_INDEX(size);
	int buffers[NR_LIST];
	int i;
	int needed;

	/* First see if we even need this.  Sometimes it is advantageous
	 to request some blocks in a filesystem that we know that we will
	 be needing ahead of time. */

	if (nr_free[isize] > 100)
		return;

	/* If there are too many dirty buffers, we wake up the update process
	   now so as to ensure that there are still clean buffers available
	   for user processes to use (and dirty) */
	
	/* We are going to try and locate this much memory */
	needed =bdf_prm.b_un.nrefill * size;  

	while (nr_free_pages > min_free_pages*2 && needed > 0 &&
	       grow_buffers(GFP_BUFFER, size)) {
		needed -= PAGE_SIZE;
	}

	if(needed <= 0) return;

	/* See if there are too many buffers of a different size.
	   If so, victimize them */

	while(maybe_shrink_lav_buffers(size))
	 {
		 if(!grow_buffers(GFP_BUFFER, size)) break;
		 needed -= PAGE_SIZE;
		 if(needed <= 0) return;
	 };

	/* OK, we cannot grow the buffer cache, now try and get some
	   from the lru list */

	/* First set the candidate pointers to usable buffers.  This
	   should be quick nearly all of the time. */

repeat0:
	for(i=0; i<NR_LIST; i++){
		if(i == BUF_DIRTY || i == BUF_SHARED || 
		   nr_buffers_type[i] == 0) {
			candidate[i] = NULL;
			buffers[i] = 0;
			continue;
		}
		buffers[i] = nr_buffers_type[i];
		for (bh = lru_list[i]; buffers[i] > 0; bh = tmp, buffers[i]--)
		 {
			 if(buffers[i] < 0) panic("Here is the problem");
			 tmp = bh->b_next_free;
			 if (!bh) break;
			 
			 if (mem_map[MAP_NR((unsigned long) bh->b_data)] != 1 ||
			     bh->b_dirt) {
				 refile_buffer(bh);
				 continue;
			 };
			 
			 if (bh->b_count || bh->b_size != size)
				  continue;
			 
			 /* Buffers are written in the order they are placed 
			    on the locked list. If we encounter a locked
			    buffer here, this means that the rest of them
			    are also locked */
			 if(bh->b_lock && (i == BUF_LOCKED || i == BUF_LOCKED1)) {
				 buffers[i] = 0;
				 break;
			 }
			 
			 if (BADNESS(bh)) continue;
			 break;
		 };
		if(!buffers[i]) candidate[i] = NULL; /* Nothing on this list */
		else candidate[i] = bh;
		if(candidate[i] && candidate[i]->b_count) panic("Here is the problem");
	}
	
 repeat:
	if(needed <= 0) return;
	
	/* Now see which candidate wins the election */
	
	winner = best_time = UINT_MAX;	
	for(i=0; i<NR_LIST; i++){
		if(!candidate[i]) continue;
		if(candidate[i]->b_lru_time < best_time){
			best_time = candidate[i]->b_lru_time;
			winner = i;
		}
	}
	
	/* If we have a winner, use it, and then get a new candidate from that list */
	if(winner != UINT_MAX) {
		i = winner;
		bh = candidate[i];
		candidate[i] = bh->b_next_free;
		if(candidate[i] == bh) candidate[i] = NULL;  /* Got last one */
		if (bh->b_count || bh->b_size != size)
			 panic("Busy buffer in candidate list\n");
		if (mem_map[MAP_NR((unsigned long) bh->b_data)] != 1)
			 panic("Shared buffer in candidate list\n");
		if (BADNESS(bh)) panic("Buffer in candidate list with BADNESS != 0\n");
		
		if(bh->b_dev == 0xffff) panic("Wrong list");
		remove_from_queues(bh);
		bh->b_dev = 0xffff;
		put_last_free(bh);
		needed -= bh->b_size;
		buffers[i]--;
		if(buffers[i] < 0) panic("Here is the problem");
		
		if(buffers[i] == 0) candidate[i] = NULL;
		
		/* Now all we need to do is advance the candidate pointer
		   from the winner list to the next usable buffer */
		if(candidate[i] && buffers[i] > 0){
			if(buffers[i] <= 0) panic("Here is another problem");
			for (bh = candidate[i]; buffers[i] > 0; bh = tmp, buffers[i]--) {
				if(buffers[i] < 0) panic("Here is the problem");
				tmp = bh->b_next_free;
				if (!bh) break;
				
				if (mem_map[MAP_NR((unsigned long) bh->b_data)] != 1 ||
				    bh->b_dirt) {
					refile_buffer(bh);
					continue;
				};
				
				if (bh->b_count || bh->b_size != size)
					 continue;
				
				/* Buffers are written in the order they are
				   placed on the locked list.  If we encounter
				   a locked buffer here, this means that the
				   rest of them are also locked */
				if(bh->b_lock && (i == BUF_LOCKED || i == BUF_LOCKED1)) {
					buffers[i] = 0;
					break;
				}
	      
				if (BADNESS(bh)) continue;
				break;
			};
			if(!buffers[i]) candidate[i] = NULL; /* Nothing here */
			else candidate[i] = bh;
			if(candidate[i] && candidate[i]->b_count) 
				 panic("Here is the problem");
		}
		
		goto repeat;
	}
	
	if(needed <= 0) return;
	
	/* Too bad, that was not enough. Try a little harder to grow some. */
	
	if (nr_free_pages > 5) {
		if (grow_buffers(GFP_BUFFER, size)) {
	                needed -= PAGE_SIZE;
			goto repeat0;
		};
	}
	
	/* and repeat until we find something good */
	if (!grow_buffers(GFP_ATOMIC, size))
		wakeup_bdflush(1);
	needed -= PAGE_SIZE;
	goto repeat0;
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algorithm is changed: hopefully better, and an elusive bug removed.
 *
 * 14.02.92: changed it to sync dirty buffers a bit: better performance
 * when the filesystem starts to get full of dirty blocks (I hope).
 */
 /*
	 缓冲区访问接口getblk()函数和brelse()函数是bcache机制向内核其它模块所提供的最重要的服务例程之一。
	 当内核需要读写某块设备上的某个块时，首先必须通过getblk()函数来得到该块在bcache中相对应的缓冲区，
	 并在使用完该缓冲区后调用brelse()释放对该缓冲区的引用。
	 */
	 /*
	 该函数得到块（dev,block）在缓冲区缓存中相应的缓冲区，大小则由参数size指定。如果相应的缓冲区还不
	 存在于bcache机制中，则必须从空闲缓冲区链表中摘取一个新项。注意！getblk()函数必须总是执行成功
 */
struct buffer_head * getblk(dev_t dev, int block, int size)
{
	struct buffer_head * bh;
        int isize = BUFSIZE_INDEX(size);

	/* Update this for the buffer size lav. */
	buffer_usage[isize]++;

	/* If there are too many dirty buffers, we wake up the update process
	   now so as to ensure that there are still clean buffers available
	   for user processes to use (and dirty) */
repeat:
	bh = get_hash_table(dev, block, size);
	if (bh) {
		if (bh->b_uptodate && !bh->b_dirt)
			 put_last_lru(bh);
		if(!bh->b_dirt) bh->b_flushtime = 0;
		return bh;
	}

	while(!free_list[isize]) refill_freelist(size);
	
	if (find_buffer(dev,block,size))
		 goto repeat;

	bh = free_list[isize];
	remove_from_free_list(bh);

/* OK, FINALLY we know that this buffer is the only one of its kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_lock=0;
	bh->b_uptodate=0;
	bh->b_flushtime = 0;
	bh->b_req=0;
	bh->b_dev=dev;
	bh->b_blocknr=block;
	insert_into_queues(bh);
	return bh;
}

void set_writetime(struct buffer_head * buf, int flag)
{
        int newtime;

	if (buf->b_dirt){
		/* Move buffer to dirty list if jiffies is clear */
		newtime = jiffies + (flag ? bdf_prm.b_un.age_super : 
				     bdf_prm.b_un.age_buffer);
		if(!buf->b_flushtime || buf->b_flushtime > newtime)
			 buf->b_flushtime = newtime;
	} else {
		buf->b_flushtime = 0;
	}
}

/*
	重新确定一个bh对象所属的lru_list链表
	当一个inuse状态下的bh对象中的b_state状态值发生变化时，就可能需要重新确定该bh对象所属的lru_list链表
	１首先，它根据b_state的值确定该bh对象应该被放置到哪一条lru_list链表中，并以变量dispose表示。
	２由于b_list值表示该bh对象当前所处的lru_list链表。因此如果dispose的值与b_list的值不相等，
	则需要将该bh对象从原来的lru_list链表中摘除，然后将他插入到新的lru_list链表中；且如果如果新
	lru_list链表是BUF_CLEAN链表，则还需要调用remove_inode_queue()函数将该bh对象从相应inode的脏缓冲区链表i_dirty_buffers中删除
*/

void refile_buffer(struct buffer_head * buf){
	int dispose;
	if(buf->b_dev == 0xffff) panic("Attempt to refile free buffer\n");
	if (buf->b_dirt)
		dispose = BUF_DIRTY;
	else if (mem_map[MAP_NR((unsigned long) buf->b_data)] > 1)
		dispose = BUF_SHARED;
	else if (buf->b_lock)
		dispose = BUF_LOCKED;
	else if (buf->b_list == BUF_SHARED)
		dispose = BUF_UNSHARED;
	else
		dispose = BUF_CLEAN;
	if(dispose == BUF_CLEAN) buf->b_lru_time = jiffies;
	if(dispose != buf->b_list)  {
		if(dispose == BUF_DIRTY || dispose == BUF_UNSHARED)
			 buf->b_lru_time = jiffies;
		if(dispose == BUF_LOCKED && 
		   (buf->b_flushtime - buf->b_lru_time) <= bdf_prm.b_un.age_super)
			 dispose = BUF_LOCKED1;
		remove_from_queues(buf);
		buf->b_list = dispose;
		insert_into_queues(buf);
		if(dispose == BUF_DIRTY && nr_buffers_type[BUF_DIRTY] > 
		   (nr_buffers - nr_buffers_type[BUF_SHARED]) *
		   bdf_prm.b_un.nfract/100)
			 wakeup_bdflush(0);
	}
}

void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);

	/* If dirty, mark the time this buffer should be written back */
	set_writetime(buf, 0);
	refile_buffer(buf);

	if (buf->b_count) {
		if (--buf->b_count)
			return;
		wake_up(&buffer_wait);
		return;
	}
	printk("VFS: brelse: Trying to free free buffer\n");
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
struct buffer_head * bread(dev_t dev, int block, int size)
{
	struct buffer_head * bh;

	if (!(bh = getblk(dev, block, size))) {
		printk("VFS: bread: READ error on device %d/%d\n",
						MAJOR(dev), MINOR(dev));
		return NULL;
	}
	if (bh->b_uptodate)
		return bh;
	ll_rw_block(READ, 1, &bh);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return NULL;
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */

#define NBUF 16

struct buffer_head * breada(dev_t dev, int block, int bufsize,
	unsigned int pos, unsigned int filesize)
{
	struct buffer_head * bhlist[NBUF];
	unsigned int blocks;
	struct buffer_head * bh;
	int index;
	int i, j;

	if (pos >= filesize)
		return NULL;

	if (block < 0 || !(bh = getblk(dev,block,bufsize)))
		return NULL;

	index = BUFSIZE_INDEX(bh->b_size);

	if (bh->b_uptodate)
		return bh;

	blocks = ((filesize & (bufsize - 1)) - (pos & (bufsize - 1))) >> (9+index);

	if (blocks > (read_ahead[MAJOR(dev)] >> index))
		blocks = read_ahead[MAJOR(dev)] >> index;
	if (blocks > NBUF)
		blocks = NBUF;
	
	bhlist[0] = bh;
	j = 1;
	for(i=1; i<blocks; i++) {
		bh = getblk(dev,block+i,bufsize);
		if (bh->b_uptodate) {
			brelse(bh);
			break;
		}
		bhlist[j++] = bh;
	}

	/* Request the read for these buffers, and then release them */
	ll_rw_block(READ, j, bhlist);

	for(i=1; i<j; i++)
		brelse(bhlist[i]);

	/* Wait for this buffer, and then continue on */
	bh = bhlist[0];
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return NULL;
}

/*
 * See fs/inode.c for the weird use of volatile..
 */
static void put_unused_buffer_head(struct buffer_head * bh)
{
	struct wait_queue * wait;

	wait = ((volatile struct buffer_head *) bh)->b_wait;
	memset(bh,0,sizeof(*bh));
	((volatile struct buffer_head *) bh)->b_wait = wait;
	bh->b_next_free = unused_list;
	unused_list = bh;
}

static void get_more_buffer_heads(void)
{
	int i;
	struct buffer_head * bh;

	if (unused_list)
		return;

	if (!(bh = (struct buffer_head*) get_free_page(GFP_BUFFER)))
		return;

	for (nr_buffer_heads+=i=PAGE_SIZE/sizeof*bh ; i>0; i--) {
		bh->b_next_free = unused_list;	/* only make link */
		unused_list = bh++;
	}
}

//从unused_list链表中或bh_cachep SLAB中得到一个bh对象，如果失败，则跳转到no_grow部分。
//注意！即使对于异步页I/O而言，get_unused_buffer_head()函数也是可能失败的。 
static struct buffer_head * get_unused_buffer_head(void)
{
	struct buffer_head * bh;

	get_more_buffer_heads();
	if (!unused_list)
		return NULL;
	bh = unused_list;
	unused_list = bh->b_next_free;
	bh->b_next_free = NULL;
	bh->b_data = NULL;
	bh->b_size = 0;
	bh->b_req = 0;
	return bh;
}

/*
 * Create the appropriate buffers when given a page for data area and
 * the size of each buffer.. Use the bh->b_this_page linked list to
 * follow the buffers created.  Return NULL if unable to create more
 * buffers.
 */
 /*
	 在所分配的物理页帧中创建空闲缓冲区，该函数返回该物理页帧中的第一个缓冲区(首地址的页内偏移为0的那个缓冲区)
	 的buffer_head对象指针。每一个buffer_head对象中的b_this_page指向该物理页帧中的下一个缓冲区，但是最后一个
	 缓冲区的buffer_head对象的b_this_page指针为NULL。 
	 */
	 /*
	 对该函数的NOTE如下： 
	(1)函数首先用一个while循环从缓冲区页的尾部开始创建缓冲区（逆续），也即从最后一个缓冲区（首地址页内偏移为PAGE_SIZE－size）到第一个缓冲区（首地址页内偏移为0）的逆续创建该缓冲区页内的缓冲区。 
	(2)调用get_unused_buffer_head()函数从unused_list链表中或bh_cachep SLAB中得到一个bh对象，如果失败，则跳转到no_grow部分。注意！即使对于异步页I/O而言，get_unused_buffer_head()函数也是可能失败的。 
	(3)如果get_unused_buffer_head()函数成功地返回一个未使用的bh对象，则对该bh对象进行初始化：(a)b_dev被设置成B_FREE，表明这是一个空闲缓冲区。(b)正确地设置b_this_page指针。注意，最后一个缓冲区的bh对象的b_this_page指针为NULL；(c)调用set_bh_page()函数设置bh对象的b_page指针和b_data指针。 
	(4)如果上述while循环成功结束，则返回第一个缓冲区的bh对象的指针。函数成功地结束。 
	(5)no_grow部分： 
	n 首先判断前面的while循环是否已经分配了部分bh对象。如果是，则通过__put_unused_buffer_head()函数将这些bh对象重新释放回bh对象缓存（unused_list链表和bh_cachep SLAB）中。然后，通过wake_up函数唤醒buffer_wait等待队列中的睡眠进程。 
	n 判断是同步I／O还是异步I／O。如果是同步I／O的话，则直接返回NULL，表示失败。如果create_buffer()函数是在为异步I／O创建缓冲区的话，那么说明有人则在使用异步缓冲区的bh对象，因此我们只有等待，然后重试即可。 
 */
static struct buffer_head * create_buffers(unsigned long page, unsigned long size)
{
	struct buffer_head *bh, *head;
	unsigned long offset;

	head = NULL;
	offset = PAGE_SIZE;
	while ((offset -= size) < PAGE_SIZE) {
		bh = get_unused_buffer_head();
		if (!bh)
			goto no_grow;
		bh->b_this_page = head;
		head = bh;
		bh->b_data = (char *) (page+offset);
		bh->b_size = size;
		bh->b_dev = 0xffff;  /* Flag as unused *///表明这是一个空闲缓冲区
	}
	return head;
/*
 * In case anything failed, we just free everything we got.
 */
no_grow:
	bh = head;
	while (bh) {
		head = bh;
		bh = bh->b_this_page;
		put_unused_buffer_head(head);
	}
	return NULL;
}

static void read_buffers(struct buffer_head * bh[], int nrbuf)
{
	int i;
	int bhnum = 0;
	struct buffer_head * bhr[MAX_BUF_PER_PAGE];

	for (i = 0 ; i < nrbuf ; i++) {
		if (bh[i] && !bh[i]->b_uptodate)
			bhr[bhnum++] = bh[i];
	}
	if (bhnum)
		ll_rw_block(READ, bhnum, bhr);
	for (i = 0 ; i < nrbuf ; i++) {
		if (bh[i]) {
			wait_on_buffer(bh[i]);
		}
	}
}

/*
 * This actually gets enough info to try to align the stuff,
 * but we don't bother yet.. We'll have to check that nobody
 * else uses the buffers etc.
 *
 * "address" points to the new page we can use to move things
 * around..
 */
static unsigned long try_to_align(struct buffer_head ** bh, int nrbuf,
	unsigned long address)
{
	while (nrbuf-- > 0)
		brelse(bh[nrbuf]);
	return 0;
}

static unsigned long check_aligned(struct buffer_head * first, unsigned long address,
	dev_t dev, int *b, int size)
{
	struct buffer_head * bh[MAX_BUF_PER_PAGE];
	unsigned long page;
	unsigned long offset;
	int block;
	int nrbuf;
	int aligned = 1;

	bh[0] = first;
	nrbuf = 1;
	page = (unsigned long) first->b_data;
	if (page & ~PAGE_MASK)
		aligned = 0;
	for (offset = size ; offset < PAGE_SIZE ; offset += size) {
		block = *++b;
		if (!block)
			goto no_go;
		first = get_hash_table(dev, block, size);
		if (!first)
			goto no_go;
		bh[nrbuf++] = first;
		if (page+offset != (unsigned long) first->b_data)
			aligned = 0;
	}
	if (!aligned)
		return try_to_align(bh, nrbuf, address);
	mem_map[MAP_NR(page)]++;
	read_buffers(bh,nrbuf);		/* make sure they are actually read correctly */
	while (nrbuf-- > 0)
		brelse(bh[nrbuf]);
	free_page(address);
	++current->mm->min_flt;
	return page;
no_go:
	while (nrbuf-- > 0)
		brelse(bh[nrbuf]);
	return 0;
}

static unsigned long try_to_load_aligned(unsigned long address,
	dev_t dev, int b[], int size)
{
	struct buffer_head * bh, * tmp, * arr[MAX_BUF_PER_PAGE];
	unsigned long offset;
        int isize = BUFSIZE_INDEX(size);
	int * p;
	int block;

	bh = create_buffers(address, size);
	if (!bh)
		return 0;
	/* do any of the buffers already exist? punt if so.. */
	p = b;
	for (offset = 0 ; offset < PAGE_SIZE ; offset += size) {
		block = *(p++);
		if (!block)
			goto not_aligned;
		if (find_buffer(dev, block, size))
			goto not_aligned;
	}
	tmp = bh;
	p = b;
	block = 0;
	while (1) {
		arr[block++] = bh;
		bh->b_count = 1;
		bh->b_dirt = 0;
		bh->b_flushtime = 0;
		bh->b_uptodate = 0;
		bh->b_req = 0;
		bh->b_dev = dev;
		bh->b_blocknr = *(p++);
		bh->b_list = BUF_CLEAN;
		nr_buffers++;
		nr_buffers_size[isize]++;
		insert_into_queues(bh);
		if (bh->b_this_page)
			bh = bh->b_this_page;
		else
			break;
	}
	buffermem += PAGE_SIZE;
	bh->b_this_page = tmp;
	mem_map[MAP_NR(address)]++;
	buffer_pages[MAP_NR(address)] = bh;
	read_buffers(arr,block);
	while (block-- > 0)
		brelse(arr[block]);
	++current->mm->maj_flt;
	return address;
not_aligned:
	while ((tmp = bh) != NULL) {
		bh = bh->b_this_page;
		put_unused_buffer_head(tmp);
	}
	return 0;
}

/*
 * Try-to-share-buffers tries to minimize memory use by trying to keep
 * both code pages and the buffer area in the same page. This is done by
 * (a) checking if the buffers are already aligned correctly in memory and
 * (b) if none of the buffer heads are in memory at all, trying to load
 * them into memory the way we want them.
 *
 * This doesn't guarantee that the memory is shared, but should under most
 * circumstances work very well indeed (ie >90% sharing of code pages on
 * demand-loadable executables).
 */
static inline unsigned long try_to_share_buffers(unsigned long address,
	dev_t dev, int *b, int size)
{
	struct buffer_head * bh;
	int block;

	block = b[0];
	if (!block)
		return 0;
	bh = get_hash_table(dev, block, size);
	if (bh)
		return check_aligned(bh, address, dev, b, size);
	return try_to_load_aligned(address, dev, b, size);
}

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc. This also allows us to optimize memory usage by sharing code pages
 * and filesystem buffers..
 */
unsigned long bread_page(unsigned long address, dev_t dev, int b[], int size, int no_share)
{
	struct buffer_head * bh[MAX_BUF_PER_PAGE];
	unsigned long where;
	int i, j;

	if (!no_share) {
		where = try_to_share_buffers(address, dev, b, size);
		if (where)
			return where;
	}
	++current->mm->maj_flt;
 	for (i=0, j=0; j<PAGE_SIZE ; i++, j+= size) {
		bh[i] = NULL;
		if (b[i])
			bh[i] = getblk(dev, b[i], size);
	}
	read_buffers(bh,i);
	where = address;
 	for (i=0, j=0; j<PAGE_SIZE ; i++, j += size, where += size) {
		if (bh[i]) {
			if (bh[i]->b_uptodate)
				memcpy((void *) where, bh[i]->b_data, size);
			brelse(bh[i]);
		}
	}
	return address;
}

/*
 * Try to increase the number of buffers available: the size argument
 * is used to determine what kind of buffers we want.
 */
static int grow_buffers(int pri, int size)
{
	unsigned long page;
	struct buffer_head *bh, *tmp;
	struct buffer_head * insert_point;
	int isize;

	if ((size & 511) || (size > PAGE_SIZE)) {
		printk("VFS: grow_buffers: size = %d\n",size);
		return 0;
	}

	isize = BUFSIZE_INDEX(size);

	if (!(page = __get_free_page(pri)))
		return 0;
	bh = create_buffers(page, size);
	if (!bh) {
		free_page(page);
		return 0;
	}

	insert_point = free_list[isize];

	tmp = bh;
	while (1) {
	        nr_free[isize]++;
		if (insert_point) {
			tmp->b_next_free = insert_point->b_next_free;
			tmp->b_prev_free = insert_point;
			insert_point->b_next_free->b_prev_free = tmp;
			insert_point->b_next_free = tmp;
		} else {
			tmp->b_prev_free = tmp;
			tmp->b_next_free = tmp;
		}
		insert_point = tmp;
		++nr_buffers;
		if (tmp->b_this_page)
			tmp = tmp->b_this_page;
		else
			break;
	}
	free_list[isize] = bh;
	buffer_pages[MAP_NR(page)] = bh;
	tmp->b_this_page = bh;
	wake_up(&buffer_wait);
	buffermem += PAGE_SIZE;
	return 1;
}


/* =========== Reduce the buffer memory ============= */

/*
 * try_to_free() checks if all the buffers on this particular page
 * are unused, and free's the page if so.
 */
 //试图释放一个缓冲区页帧物理页面
 //一个我认为比较重要的问题，如果内核刚将数据块从设备读入缓冲区
 //在还没有进行要操作的操作之前就将缓冲块释放了....
 //参见block_dec.c文件block_write函数中的相关注释
 /*
	上面提到的问题应该不会存在 因为内核所使用的对象都有一个引用计数
	内核不会将引用计数大于0的对象释放 内核将数据读入缓冲区 会增加相
	应的缓冲区对象的引用计数
 */
static int try_to_free(struct buffer_head * bh, struct buffer_head ** bhp)
{
	unsigned long page;
	struct buffer_head * tmp, * p;
    int isize = BUFSIZE_INDEX(bh->b_size);

	*bhp = bh;
	page = (unsigned long) bh->b_data;
	page &= PAGE_MASK;
	tmp = bh;
	//循环遍历此页帧内的缓冲区 若有被占用的或者脏的或者....则返回0
	do {
		if (!tmp)
			return 0;
		if (tmp->b_count || tmp->b_dirt || tmp->b_lock || tmp->b_wait)
			return 0;
		tmp = tmp->b_this_page;
	} while (tmp != bh);
	tmp = bh;
	//再次循环遍历此页帧内的缓冲区 清除缓冲区结构
	do {
		p = tmp;
		tmp = tmp->b_this_page;
		nr_buffers--;
		nr_buffers_size[isize]--;
		if (p == *bhp)
		  {
		    *bhp = p->b_prev_free;
		    if (p == *bhp) /* Was this the last in the list? */
		      *bhp = NULL;
		  }
		remove_from_queues(p);
		put_unused_buffer_head(p);
	} while (tmp != bh);
	buffermem -= PAGE_SIZE;
	buffer_pages[MAP_NR(page)] = NULL;
	free_page(page);
	return !mem_map[MAP_NR(page)];
}


/*
 * Consult the load average for buffers and decide whether or not
 * we should shrink the buffers of one size or not.  If we decide yes,
 * do it and return 1.  Else return 0.  Do not attempt to shrink size
 * that is specified.
 *
 * I would prefer not to use a load average, but the way things are now it
 * seems unavoidable.  The way to get rid of it would be to force clustering
 * universally, so that when we reclaim buffers we always reclaim an entire
 * page.  Doing this would mean that we all need to move towards QMAGIC.
 */

static int maybe_shrink_lav_buffers(int size)
{	   
	int nlist;
	int isize;
	int total_lav, total_n_buffers, n_sizes;
	
	/* Do not consider the shared buffers since they would not tend
	   to have getblk called very often, and this would throw off
	   the lav.  They are not easily reclaimable anyway (let the swapper
	   make the first move). */
  
	total_lav = total_n_buffers = n_sizes = 0;
	for(nlist = 0; nlist < NR_SIZES; nlist++)
	 {
		 total_lav += buffers_lav[nlist];
		 if(nr_buffers_size[nlist]) n_sizes++;
		 total_n_buffers += nr_buffers_size[nlist];
		 total_n_buffers -= nr_buffers_st[nlist][BUF_SHARED]; 
	 }
	
	/* See if we have an excessive number of buffers of a particular
	   size - if so, victimize that bunch. */
  
	isize = (size ? BUFSIZE_INDEX(size) : -1);
	
	if (n_sizes > 1)
		 for(nlist = 0; nlist < NR_SIZES; nlist++)
		  {
			  if(nlist == isize) continue;
			  if(nr_buffers_size[nlist] &&
			     bdf_prm.b_un.lav_const * buffers_lav[nlist]*total_n_buffers < 
			     total_lav * (nr_buffers_size[nlist] - nr_buffers_st[nlist][BUF_SHARED]))
				   if(shrink_specific_buffers(6, bufferindex_size[nlist])) 
					    return 1;
		  }
	return 0;
}
/*
 * Try to free up some pages by shrinking the buffer-cache
 *
 * Priority tells the routine how hard to try to shrink the
 * buffers: 3 means "don't bother too much", while a value
 * of 0 means "we'd better get some free pages now".
 */
int shrink_buffers(unsigned int priority)
{
	if (priority < 2) {
		sync_buffers(0,0);
	}

	if(priority == 2) wakeup_bdflush(1);

	if(maybe_shrink_lav_buffers(0)) return 1;

	/* No good candidate size - take any size we can find */
        return shrink_specific_buffers(priority, 0);
}

static int shrink_specific_buffers(unsigned int priority, int size)
{
	struct buffer_head *bh;
	int nlist;
	int i, isize, isize1;

#ifdef DEBUG
	if(size) printk("Shrinking buffers of size %d\n", size);
#endif
	/* First try the free lists, and see if we can get a complete page
	   from here */
	isize1 = (size ? BUFSIZE_INDEX(size) : -1);

	for(isize = 0; isize<NR_SIZES; isize++){
		if(isize1 != -1 && isize1 != isize) continue;
		bh = free_list[isize];
		if(!bh) continue;
		for (i=0 ; !i || bh != free_list[isize]; bh = bh->b_next_free, i++) {
			if (bh->b_count || !bh->b_this_page)
				 continue;
			if (try_to_free(bh, &bh))
				 return 1;
			if(!bh) break; /* Some interrupt must have used it after we
					  freed the page.  No big deal - keep looking */
		}
	}
	
	/* Not enough in the free lists, now try the lru list */
	
	for(nlist = 0; nlist < NR_LIST; nlist++) {
	repeat1:
		if(priority > 3 && nlist == BUF_SHARED) continue;
		bh = lru_list[nlist];
		if(!bh) continue;
		i = 2*nr_buffers_type[nlist] >> priority;
		for ( ; i-- > 0 ; bh = bh->b_next_free) {
			/* We may have stalled while waiting for I/O to complete. */
			if(bh->b_list != nlist) goto repeat1;
			if (bh->b_count || !bh->b_this_page)
				 continue;
			if(size && bh->b_size != size) continue;
			if (bh->b_lock)
				 if (priority)
					  continue;
				 else
					  wait_on_buffer(bh);
			if (bh->b_dirt) {
				bh->b_count++;
				bh->b_flushtime = 0;
				ll_rw_block(WRITEA, 1, &bh);
				bh->b_count--;
				continue;
			}
			if (try_to_free(bh, &bh))
				 return 1;
			if(!bh) break;
		}
	}
	return 0;
}


/* ================== Debugging =================== */

void show_buffers(void)
{
	struct buffer_head * bh;
	int found = 0, locked = 0, dirty = 0, used = 0, lastused = 0;
	int shared;
	int nlist, isize;

	printk("Buffer memory:   %6dkB\n",buffermem>>10);
	printk("Buffer heads:    %6d\n",nr_buffer_heads);
	printk("Buffer blocks:   %6d\n",nr_buffers);

	for(nlist = 0; nlist < NR_LIST; nlist++) {
	  shared = found = locked = dirty = used = lastused = 0;
	  bh = lru_list[nlist];
	  if(!bh) continue;
	  do {
		found++;
		if (bh->b_lock)
			locked++;
		if (bh->b_dirt)
			dirty++;
		if(mem_map[MAP_NR(((unsigned long) bh->b_data))] !=1) shared++;
		if (bh->b_count)
			used++, lastused = found;
		bh = bh->b_next_free;
	      } while (bh != lru_list[nlist]);
	printk("Buffer[%d] mem: %d buffers, %d used (last=%d), %d locked, %d dirty %d shrd\n",
		nlist, found, used, lastused, locked, dirty, shared);
	};
	printk("Size    [LAV]     Free  Clean  Unshar     Lck    Lck1   Dirty  Shared\n");
	for(isize = 0; isize<NR_SIZES; isize++){
		printk("%5d [%5d]: %7d ", bufferindex_size[isize],
		       buffers_lav[isize], nr_free[isize]);
		for(nlist = 0; nlist < NR_LIST; nlist++)
			 printk("%7d ", nr_buffers_st[isize][nlist]);
		printk("\n");
	}
}


/* ====================== Cluster patches for ext2 ==================== */

/*
 * try_to_reassign() checks if all the buffers on this particular page
 * are unused, and reassign to a new cluster them if this is true.
 */
 //此函数检查指定的buffer所在的物理页面内的所有buffer是不是unused状态，
 //如果是，则将他们重新组合为一个新的cluster（簇），即将页面内的buffer
 //所对应的设备块号设置为一组从starting_block开始连续的值
 //将块号连续的块放在同一个页面，可能效率要高？??
static inline int try_to_reassign(struct buffer_head * bh, struct buffer_head ** bhp,
			   dev_t dev, unsigned int starting_block)
{
	unsigned long page;
	struct buffer_head * tmp, * p;

	*bhp = bh;
	//获取该buffer所在物理页面
	page = (unsigned long) bh->b_data;
	page &= PAGE_MASK;
	//如果该页面被共享，返回0
	if(mem_map[MAP_NR(page)] != 1) return 0;
	tmp = bh;
	do {
		//若tmp为空，则说明此页面内没有填满buffer，则返回0
		if (!tmp)
			 return 0;
		//如果该buffer被引用了，或者脏了，或者上锁了，返回0
		if (tmp->b_count || tmp->b_dirt || tmp->b_lock)
			 return 0;
		//tmp指向该页面内的下一个buffer
		tmp = tmp->b_this_page;
	} while (tmp != bh);
	tmp = bh;
	
	//To find the buffer at the head of the page
	while((unsigned long) tmp->b_data & (PAGE_SIZE - 1)) 
		 tmp = tmp->b_this_page;
	
	/* This is the buffer at the head of the page */
	bh = tmp;
	do {
		//p指向当前buffer
		p = tmp;
		//tmp指向本页面内p所指向的buffer的下一个buffer
		tmp = tmp->b_this_page;
		//将p所指向的buffer从队列中移除
		remove_from_queues(p);
		p->b_dev=dev;
		//将p所指向的buffer内的数据设为无效
		p->b_uptodate = 0;
		p->b_req = 0;
		p->b_blocknr=starting_block++;
		//将p所指向的buffer重新插入队列
		insert_into_queues(p);
	} while (tmp != bh);
	//返回1，表示成功组合为一个新的簇
	return 1;
}

/*
 * Try to find a free cluster by locating a page where
 * all of the buffers are unused.  We would like this function
 * to be atomic, so we do not call anything that might cause
 * the process to sleep.  The priority is somewhat similar to
 * the priority used in shrink_buffers.
 * 
 * My thinking is that the kernel should end up using whole
 * pages for the buffer cache as much of the time as possible.
 * This way the other buffers on a particular page are likely
 * to be very near each other on the free list, and we will not
 * be expiring data prematurely.  For now we only cannibalize buffers
 * of the same size to keep the code simpler.
 */
static int reassign_cluster(dev_t dev, unsigned int starting_block, int size)
{
	struct buffer_head *bh;
    int isize = BUFSIZE_INDEX(size);
	int i;

	/* We want to give ourselves a really good shot at generating
	   a cluster, and since we only take buffers from the free
	   list, we "overfill" it a little. */

	while(nr_free[isize] < 32) refill_freelist(size);

	bh = free_list[isize];
	if(bh)
		 for (i=0 ; !i || bh != free_list[isize] ; bh = bh->b_next_free, i++) {
			//如果此buffer的b_this_page为空，说明其所在页面未填满buffer，则continue
			//此判断也可以是"多余的"，因为try_to_reassign函数中会对此进行判断
			//但此判断可减少不必要的函数调用
			 if (!bh->b_this_page)	continue;
			 if (try_to_reassign(bh, &bh, dev, starting_block))
				 return 4;
		 }
	return 0;
}

/* This function tries to generate a new cluster of buffers
 * from a new page in memory.  We should only do this if we have
 * not expanded the buffer cache to the maximum size that we allow.
 */
 //将块号连续的块放在同一个页面，可能效率要高？
static unsigned long try_to_generate_cluster(dev_t dev, int block, int size)
{
	struct buffer_head * bh, * tmp, * arr[MAX_BUF_PER_PAGE];
    int isize = BUFSIZE_INDEX(size);
	unsigned long offset;
	unsigned long page;
	int nblock;

	page = get_free_page(GFP_NOBUFFER);
	if(!page) return 0;

	bh = create_buffers(page, size);
	if (!bh) {
		free_page(page);
		return 0;
	};
	nblock = block;
	for (offset = 0 ; offset < PAGE_SIZE ; offset += size) {
		//如果当前缓冲区中包含要组成一簇buffer的块号。。。
		if (find_buffer(dev, nblock++, size))
			 goto not_aligned;
	}
	tmp = bh;
	nblock = 0;
	while (1) {
		//arr数组大小就是一页物理内存所能容纳的最大buffer数
		arr[nblock++] = bh;
		bh->b_count = 1;
		bh->b_dirt = 0;
		bh->b_flushtime = 0;
		bh->b_lock = 0;
		bh->b_uptodate = 0;
		bh->b_req = 0;
		bh->b_dev = dev;
		bh->b_list = BUF_CLEAN;
		bh->b_blocknr = block++;
		nr_buffers++;
		nr_buffers_size[isize]++;
		insert_into_queues(bh);
		if (bh->b_this_page)
			bh = bh->b_this_page;
		else
			break;
	}
	buffermem += PAGE_SIZE;
	buffer_pages[MAP_NR(page)] = bh;
	//将页面内的buffer链成一个循环链表
	bh->b_this_page = tmp;
	while (nblock-- > 0)
		brelse(arr[nblock]);
	return 4; /* ?? */
not_aligned:
	while ((tmp = bh) != NULL) {
		bh = bh->b_this_page;
		put_unused_buffer_head(tmp);
	}
	free_page(page);
	return 0;
}

unsigned long generate_cluster(dev_t dev, int b[], int size)
{
	int i, offset;
	
	for (i = 0, offset = 0 ; offset < PAGE_SIZE ; i++, offset += size) {
		//b数组内存储的块号是不是连续的，若不是，则返回0
		if(i && b[i]-1 != b[i-1]) return 0;  /* No need to cluster */
		//如果在缓冲中找到b数组中存储的块号，返回0
		if(find_buffer(dev, b[i], size)) return 0;
	};

	/* OK, we have a candidate for a new cluster */
	
	/* See if one size of buffer is over-represented in the buffer cache,
	   if so reduce the numbers of buffers */
	if(maybe_shrink_lav_buffers(size))
	 {
		 int retval;
		 retval = try_to_generate_cluster(dev, b[0], size);
		 if(retval) return retval;
	 };
	
	if (nr_free_pages > min_free_pages*2) 
		 return try_to_generate_cluster(dev, b[0], size);
	else
		 return reassign_cluster(dev, b[0], size);
}


/* ===================== Init ======================= */

/*
 * This initializes the initial buffer free list.  nr_buffers_type is set
 * to one less the actual number of buffers, as a sop to backwards
 * compatibility --- the old code did this (I think unintentionally,
 * but I'm not sure), and programs in the ps package expect it.
 * 					- TYT 8/30/92
 */
 
 /*
	 函数buffer_init()完成bcache机制的初始化工作。它主要完成三件事：
	（1）为buffer_head哈希链表表头数组分配内存，并对其进行初始化；
	（2）初始化free_list链表数组中的元素；
	（3）初始化lru_list链表表头数组中的元素。
 */
 
void buffer_init(void)
{
	int i;
        int isize = BUFSIZE_INDEX(BLOCK_SIZE);
	//根据实际物理内存大小确定缓冲哈希表数组大小
	if (high_memory >= 4*1024*1024) {
		if(high_memory >= 16*1024*1024)
			 nr_hash = 16381;
		else
			 nr_hash = 4093;
	} else {
		nr_hash = 997;
	};
	
	//用vmalloc函数为内核分配大量虚拟地址连续的内存
	hash_table = (struct buffer_head **) vmalloc(nr_hash * 
						     sizeof(struct buffer_head *));


	buffer_pages = (struct buffer_head **) vmalloc(MAP_NR(high_memory) * 
						     sizeof(struct buffer_head *));
	for (i = 0 ; i < MAP_NR(high_memory) ; i++)
		buffer_pages[i] = NULL;

	//置空哈希数组表项
	for (i = 0 ; i < nr_hash ; i++)
		hash_table[i] = NULL;
	lru_list[BUF_CLEAN] = 0;	//置空未使用的、干净的缓冲区类型的缓冲区链表
	grow_buffers(GFP_KERNEL, BLOCK_SIZE);
	if (!free_list[isize])
		panic("VFS: Unable to initialize buffer free list!");
	return;
}


/* ====================== bdflush support =================== */

/* This is a simple kernel daemon, whose job it is to provide a dynamically
 * response to dirty buffers.  Once this process is activated, we write back
 * a limited number of buffers to the disks and then go back to sleep again.
 * In effect this is a process which never leaves kernel mode, and does not have
 * any user memory associated with it except for the stack.  There is also
 * a kernel stack page, which obviously must be separate from the user stack.
 */
struct wait_queue * bdflush_wait = NULL;
struct wait_queue * bdflush_done = NULL;

static int bdflush_running = 0;

static void wakeup_bdflush(int wait)
{
	if(!bdflush_running){
		printk("Warning - bdflush not running\n");
		sync_buffers(0,0);
		return;
	};
	wake_up(&bdflush_wait);
	if(wait) sleep_on(&bdflush_done);
}



/* 
 * Here we attempt to write back old buffers.  We also try and flush inodes 
 * and supers as well, since this function is essentially "update", and 
 * otherwise there would be no way of ensuring that these quantities ever 
 * get written back.  Ideally, we would have a timestamp on the inodes
 * and superblocks so that we could write back only the old ones as well
 */

asmlinkage int sync_old_buffers(void)
{
	int i, isize;
	int ndirty, nwritten;
	int nlist;
	int ncount;
	struct buffer_head * bh, *next;

	sync_supers(0);
	sync_inodes(0);

	ncount = 0;
#ifdef DEBUG
	for(nlist = 0; nlist < NR_LIST; nlist++)
#else
	for(nlist = BUF_DIRTY; nlist <= BUF_DIRTY; nlist++)
#endif
	{
		ndirty = 0;
		nwritten = 0;
	repeat:
		bh = lru_list[nlist];
		if(bh) 
			 for (i = nr_buffers_type[nlist]; i-- > 0; bh = next) {
				 /* We may have stalled while waiting for I/O to complete. */
				 if(bh->b_list != nlist) goto repeat;
				 next = bh->b_next_free;
				 if(!lru_list[nlist]) {
					 printk("Dirty list empty %d\n", i);
					 break;
				 }
				 
				 /* Clean buffer on dirty list?  Refile it */
				 if (nlist == BUF_DIRTY && !bh->b_dirt && !bh->b_lock)
				  {
					  refile_buffer(bh);
					  continue;
				  }
				 
				 if (bh->b_lock || !bh->b_dirt)
					  continue;
				 ndirty++;
				 if(bh->b_flushtime > jiffies) continue;
				 nwritten++;
				 bh->b_count++;
				 bh->b_flushtime = 0;
#ifdef DEBUG
				 if(nlist != BUF_DIRTY) ncount++;
#endif
				 ll_rw_block(WRITE, 1, &bh);
				 bh->b_count--;
			 }
	}
#ifdef DEBUG
	if (ncount) printk("sync_old_buffers: %d dirty buffers not on dirty list\n", ncount);
	printk("Wrote %d/%d buffers\n", nwritten, ndirty);
#endif
	
	/* We assume that we only come through here on a regular
	   schedule, like every 5 seconds.  Now update load averages.  
	   Shift usage counts to prevent overflow. */
	for(isize = 0; isize<NR_SIZES; isize++){
		CALC_LOAD(buffers_lav[isize], bdf_prm.b_un.lav_const, buffer_usage[isize]);
		buffer_usage[isize] = 0;
	};
	return 0;
}


/* This is the interface to bdflush.  As we get more sophisticated, we can
 * pass tuning parameters to this "process", to adjust how it behaves.  If you
 * invoke this again after you have done this once, you would simply modify 
 * the tuning parameters.  We would want to verify each parameter, however,
 * to make sure that it is reasonable. */

asmlinkage int sys_bdflush(int func, long data)
{
	int i, error;
	int ndirty;
	int nlist;
	int ncount;
	struct buffer_head * bh, *next;

	if (!suser())
		return -EPERM;

	if (func == 1)
		 return sync_old_buffers();

	/* Basically func 0 means start, 1 means read param 1, 2 means write param 1, etc */
	if (func >= 2) {
		i = (func-2) >> 1;
		if (i < 0 || i >= N_PARAM)
			return -EINVAL;
		if((func & 1) == 0) {
			error = verify_area(VERIFY_WRITE, (void *) data, sizeof(int));
			if (error)
				return error;
			put_fs_long(bdf_prm.data[i], data);
			return 0;
		};
		if (data < bdflush_min[i] || data > bdflush_max[i])
			return -EINVAL;
		bdf_prm.data[i] = data;
		return 0;
	};
	
	if (bdflush_running)
		return -EBUSY; /* Only one copy of this running at one time */
	bdflush_running++;
	
	/* OK, from here on is the daemon */
	
	for (;;) {
#ifdef DEBUG
		printk("bdflush() activated...");
#endif
		
		ncount = 0;
#ifdef DEBUG
		for(nlist = 0; nlist < NR_LIST; nlist++)
#else
		for(nlist = BUF_DIRTY; nlist <= BUF_DIRTY; nlist++)
#endif
		 {
			 ndirty = 0;
		 repeat:
			 bh = lru_list[nlist];
			 if(bh) 
				  for (i = nr_buffers_type[nlist]; i-- > 0 && ndirty < bdf_prm.b_un.ndirty; 
				       bh = next) {
					  /* We may have stalled while waiting for I/O to complete. */
					  if(bh->b_list != nlist) goto repeat;
					  next = bh->b_next_free;
					  if(!lru_list[nlist]) {
						  printk("Dirty list empty %d\n", i);
						  break;
					  }
					  
					  /* Clean buffer on dirty list?  Refile it */
					  if (nlist == BUF_DIRTY && !bh->b_dirt && !bh->b_lock)
					   {
						   refile_buffer(bh);
						   continue;
					   }
					  
					  if (bh->b_lock || !bh->b_dirt)
						   continue;
					  /* Should we write back buffers that are shared or not??
					     currently dirty buffers are not shared, so it does not matter */
					  bh->b_count++;
					  ndirty++;
					  bh->b_flushtime = 0;
					  ll_rw_block(WRITE, 1, &bh);
#ifdef DEBUG
					  if(nlist != BUF_DIRTY) ncount++;
#endif
					  bh->b_count--;
				  }
		 }
#ifdef DEBUG
		if (ncount) printk("sys_bdflush: %d dirty buffers not on dirty list\n", ncount);
		printk("sleeping again.\n");
#endif
		wake_up(&bdflush_done);
		
		/* If there are still a lot of dirty buffers around, skip the sleep
		   and flush some more */
		
		if(nr_buffers_type[BUF_DIRTY] < (nr_buffers - nr_buffers_type[BUF_SHARED]) * 
		   bdf_prm.b_un.nfract/100) {
		   	if (current->signal & (1 << (SIGKILL-1))) {
				bdflush_running--;
		   		return 0;
			}
		   	current->signal = 0;
			interruptible_sleep_on(&bdflush_wait);
		}
	}
}


/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 8
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -8
 * c-argdecl-indent: 8
 * c-label-offset: -8
 * c-continued-statement-offset: 8
 * c-continued-brace-offset: 0
 * End:
 */
