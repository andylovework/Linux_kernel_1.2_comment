#define THREE_LEVEL
/*
 *  linux/mm/memory.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

/*
 * Real VM (paging to/from disk) started 18.12.91. Much more work and
 * thought has to go into this. Oh, well..
 * 19.12.91  -  works, somewhat. Sometimes I get faults, don't know why.
 *		Found it. Everything seems to work now.
 * 20.12.91  -  Ok, making the swap-device changeable like the root.
 */

/*
 * 05.04.94  -  Multi-page memory management added for v1.1.
 * 		Idea by Alex Bligh (alex@cconcepts.co.uk)
 */

#include <linux/config.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/pgtable.h>

/*
						关于high_memory
	每个进程都有自己的私有用户空间（0-3GB），这个空间对系统中的其他进程是不可见的。最高的1GB内核空间
	则由则由所有进程以及内核共享。可见，内核最多寻址1G的虚拟地址空间。Linux 内核采用了最简单的映射方
	式来映射物理内存，即把物理地址＋PAGE_OFFSET按照线性关系直接映射到内核空间。PAGE_OFFSET大小为0xC000000.
	但是linux内核并没有把整个1G空间用于线性映射，而只映射了最多896M物理内存，预留了最高端的128M虚拟地址空间
	给IO设备和其他用途。所以，当系统物理内存较大时，超过896M的内存区域，内核就无法直接通过线性映射直接访问了。
	这部分内存被称作high memory。相应的可以映射的低端物理内存称为Low memory.而对于2G/2G划分的arm机器，这个线
	性映射空间就可能达到1G以上，能够直接映射到这个线性空间的物理地址是DMA zone和Normal Zone，在这个范围之外的
	物理内存则划归High memory Zone。
结论：
	1）high memory针对的是物理内存，不是虚拟内存。 
	2）high memory也是被内核管理的（有对应的page结构），只是没有映射到内核虚拟地址空间。
	  当内核需要分配high memory时，通过kmap等从预留的地址空间中动态分配一个地址，然后映射
	  到high memory，从而访问这个物理页。high memory映射到内核地址空间一般是暂时性的映射，
	  不是永久映射。 
	3）high memory和low memory一样，都是参与内核的物理内存分配，都可以被映射到内核地址空间，
	   也都可以被映射到用户地址空间。 
	4）物理内存<896M时，没有high memory，因为所有的内存都被kernel直接映射了。 
	5）64位系统下不会有high memory，因为64位虚拟地址空间非常大（分给kernel的也很大），
	   完全能够直接映射全部物理内存。
*/

unsigned long high_memory = 0;

/*
 * The free_area_list arrays point to the queue heads of the free areas
 * of different sizes
 */
int nr_swap_pages = 0;
int nr_free_pages = 0;
struct mem_list free_area_list[NR_MEM_LISTS];
unsigned char * free_area_map[NR_MEM_LISTS];

#define copy_page(from,to) memcpy((void *) to, (void *) from, PAGE_SIZE)

#define USER_PTRS_PER_PGD (TASK_SIZE / PGDIR_SIZE)

mem_map_t * mem_map = NULL;

/*
 * oom() prints a message (so that the user knows why the process died),
 * and gives the process an untrappable SIGKILL.
 */
void oom(struct task_struct * task)
{
	printk("\nOut of memory for %s.\n", current->comm);
	task->sigaction[SIGKILL-1].sa_handler = NULL;
	task->blocked &= ~(1<<(SIGKILL-1));
	send_sig(SIGKILL,task,1);
}

//释放一项页表所指向的物理内存页面
static inline void free_one_pte(pte_t * page_table)
{
	pte_t page = *page_table;

	if (pte_none(page))
		return;
	pte_clear(page_table);
	//如果此物理页面不在内存中，则调用swap_free，否则调用free_page直接释放此物理页面
	if (!pte_present(page)) {
		swap_free(pte_val(page));
		return;
	}
	free_page(pte_page(page));
	return;
}

//释放一项二级页表所指向的所有物理内存页面
static inline void free_one_pmd(pmd_t * dir)
{
	int j;
	pte_t * pte;

	if (pmd_none(*dir))
		return;
	if (pmd_bad(*dir)) {
		printk("free_one_pmd: bad directory entry %08lx\n", pmd_val(*dir));
		pmd_clear(dir);
		return;
	}
	pte = pte_offset(dir, 0);	//得到三级页表所在物理页面首指针
	pmd_clear(dir);
	//如果此pte页面是被共享的，则调用pte_free释放，减少页面引用次数
	if (pte_inuse(pte)) {
		pte_free(pte);
		return;
	}
	//程序执行到这里，说明三级页表是由程序独享的，则调用free_one_pte一项一项的释放页表中每个指针所指向的物理页面
	for (j = 0; j < PTRS_PER_PTE ; j++)
		free_one_pte(pte+j);
	pte_free(pte);
}


//释放一项一级页目录表所指向的所有物理内存页面
static inline void free_one_pgd(pgd_t * dir)
{
	int j;
	pmd_t * pmd;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		printk("free_one_pgd: bad directory entry %08lx\n", pgd_val(*dir));
		pgd_clear(dir);
		return;
	}
	pmd = pmd_offset(dir, 0);	//取得此目录项所指向的二级页表（中间页面）所在物理页面首指针
	pgd_clear(dir);
	if (pmd_inuse(pmd)) {
		pmd_free(pmd);
		return;
	}
	for (j = 0; j < PTRS_PER_PMD ; j++)
		free_one_pmd(pmd+j);
	pmd_free(pmd);
}
	

/*
 * This function clears all user-level page tables of a process - this
 * is needed by execve(), so that old pages aren't in the way. Note that
 * unlike 'free_page_tables()', this function still leaves a valid
 * page-table-tree in memory: it just removes the user pages. The two
 * functions are similar, but there is a fundamental difference.
 */
void clear_page_tables(struct task_struct * tsk)
{
	int i;
	pgd_t * page_dir;

	if (!tsk)
		return;
	if (tsk == task[0])
		panic("task[0] (swapper) doesn't support exec()\n");
	page_dir = pgd_offset(tsk, 0);
	if (!page_dir || page_dir == swapper_pg_dir) {
		printk("Trying to clear kernel page-directory: not good\n");
		return;
	}
	if (pgd_inuse(page_dir)) {
		pgd_t * new_pg;
		if (!(new_pg = pgd_alloc())) {
			oom(tsk);
			return;
		}
		//因为要清除此进程所有的用户级别的页表并且此page_dir是共享内存，所以重新申请了一页物理内存作为此进程的新页目录表
		//由于新申请的页面做了清零操作，所以只将内核级页表项复制到新新页目录表中就可以了
		for (i = USER_PTRS_PER_PGD ; i < PTRS_PER_PGD ; i++)
			new_pg[i] = page_dir[i];
		SET_PAGE_DIR(tsk, new_pg);	//将此新页目录表设为此进程的页目录表
		pgd_free(page_dir);		//释放旧的废弃的进程页目录表
		return;
	}
	//执行到这里，说明页表是由进程独享的，所以可以直接释放掉进程页目录表中的用户级页目录表项
	for (i = 0 ; i < USER_PTRS_PER_PGD ; i++)
		free_one_pgd(page_dir + i);
	invalidate();	//刷新TLB
	return;
}

/*
 * This function frees up all page tables of a process when it exits.
 */
void free_page_tables(struct task_struct * tsk)
{
	int i;
	pgd_t * page_dir;

	if (!tsk)
		return;
	if (tsk == task[0]) {
		printk("task[0] (swapper) killed: unable to recover\n");
		panic("Trying to free up swapper memory space");
	}
	page_dir = pgd_offset(tsk, 0);
	if (!page_dir || page_dir == swapper_pg_dir) {
		printk("Trying to free kernel page-directory: not good\n");
		return;
	}
	SET_PAGE_DIR(tsk, swapper_pg_dir);
	if (pgd_inuse(page_dir)) {
		pgd_free(page_dir);
		return;
	}
	for (i = 0 ; i < PTRS_PER_PGD ; i++)
		free_one_pgd(page_dir + i);
	pgd_free(page_dir);	
	invalidate();
}

/*
 * clone_page_tables() clones the page table for a process - both
 * processes will have the exact same pages in memory. There are
 * probably races in the memory management with cloning, but we'll
 * see..
 */
int clone_page_tables(struct task_struct * tsk)
{
	pgd_t * pg_dir;

	pg_dir = pgd_offset(current, 0);
	pgd_reuse(pg_dir);	//只是增加pg_dir所在物理页面在mem_map数组中的引用计数
	SET_PAGE_DIR(tsk, pg_dir);
	return 0;
}

static inline void copy_one_pte(pte_t * old_pte, pte_t * new_pte)
{
	pte_t pte = *old_pte;

	if (pte_none(pte))
		return;
	if (!pte_present(pte)) {
		swap_duplicate(pte_val(pte));
		*new_pte = pte;
		return;
	}
	//如果pte超过了high_memory（在宏pmd_bad中若pmd>high_memory,就返回真）或者其带有MAP_PAGE_RESERVED标志属性，
	//说明其是内核空间或者...可以由所有应用程序共享，所以直接复制给new_pte
	if (pte_page(pte) > high_memory || (mem_map[MAP_NR(pte_page(pte))] & MAP_PAGE_RESERVED)) {
		*new_pte = pte;
		return;
	}
	//如果pte中带有copy on write的标志，则设置pte的读写保护标志
	if (pte_cow(pte))
		pte = pte_wrprotect(pte);
	if (delete_from_swap_cache(pte_page(pte)))
		pte = pte_mkdirty(pte);
	*new_pte = pte_mkold(pte);
	*old_pte = pte;
	//执行到这里说明要复制的页表项指向的物理页面不是内核页面，所以共享时要增加其在mem_map数组中的引用计数
	mem_map[MAP_NR(pte_page(pte))]++;
}

static inline int copy_one_pmd(pmd_t * old_pmd, pmd_t * new_pmd)
{
	int j;
	pte_t *old_pte, *new_pte;

	if (pmd_none(*old_pmd))
		return 0;
	if (pmd_bad(*old_pmd)) {
		printk("copy_one_pmd: bad page table: probable memory corruption\n");
		pmd_clear(old_pmd);
		return 0;
	}
	old_pte = pte_offset(old_pmd, 0);	//取得pmd所指向的三级页表所在物理页面首地址
	//若页表页面是共享页面，则增加此页面在mem_map中的引用计数，并复制到new_pmd中
	if (pte_inuse(old_pte)) {
		pte_reuse(old_pte);
		*new_pmd = *old_pmd;
		return 0;
	}
	//否则，申请一页新的页面，并将pmd所指向的三级页表中的页表项一项一项的复制到new_pte页面中来，why？
	new_pte = pte_alloc(new_pmd, 0);	//pte_alloc和pte_alloc_kernel的区别是，后者申请的页面带有共享标志
	if (!new_pte)
		return -ENOMEM;
	for (j = 0 ; j < PTRS_PER_PTE ; j++) {
		copy_one_pte(old_pte, new_pte);
		old_pte++;
		new_pte++;
	}
	return 0;
}

static inline int copy_one_pgd(pgd_t * old_pgd, pgd_t * new_pgd)
{
	int j;
	pmd_t *old_pmd, *new_pmd;

	if (pgd_none(*old_pgd))
		return 0;
	if (pgd_bad(*old_pgd)) {
		printk("copy_one_pgd: bad page table (%p: %08lx): probable memory corruption\n", old_pgd, pgd_val(*old_pgd));
		pgd_clear(old_pgd);
		return 0;
	}
	old_pmd = pmd_offset(old_pgd, 0);
	if (pmd_inuse(old_pmd)) {
		pmd_reuse(old_pmd);
		*new_pgd = *old_pgd;
		return 0;
	}
	new_pmd = pmd_alloc(new_pgd, 0);
	if (!new_pmd)
		return -ENOMEM;
	for (j = 0 ; j < PTRS_PER_PMD ; j++) {
		int error = copy_one_pmd(old_pmd, new_pmd);
		if (error)
			return error;
		old_pmd++;
		new_pmd++;
	}
	return 0;
}

/*
 * copy_page_tables() just copies the whole process memory range:
 * note the special handling of RESERVED (	ie kernel) pages, which
 * means that they are always shared by all processes.
 */
int copy_page_tables(struct task_struct * tsk)
{
	int i;
	pgd_t *old_pgd;
	pgd_t *new_pgd;

	new_pgd = pgd_alloc();
	if (!new_pgd)
		return -ENOMEM;
	SET_PAGE_DIR(tsk, new_pgd);
	old_pgd = pgd_offset(current, 0);
	//页目录表页面可能常驻内存
	for (i = 0 ; i < PTRS_PER_PGD ; i++) {
		int errno = copy_one_pgd(old_pgd, new_pgd);
		if (errno) {
			free_page_tables(tsk);
			invalidate();
			return errno;
		}
		old_pgd++;
		new_pgd++;
	}
	invalidate();
	return 0;
}

static inline void forget_pte(pte_t page)
{
	if (pte_none(page))
		return;
	if (pte_present(page)) {
		free_page(pte_page(page));
		if (mem_map[MAP_NR(pte_page(page))] & MAP_PAGE_RESERVED)
			return;
		if (current->mm->rss <= 0)
			return;
		current->mm->rss--;
		return;
	}
	swap_free(pte_val(page));
}

static inline void unmap_pte_range(pmd_t * pmd, unsigned long address, unsigned long size)
{
	pte_t * pte;
	unsigned long end;

	if (pmd_none(*pmd))
		return;
	if (pmd_bad(*pmd)) {
		printk("unmap_pte_range: bad pmd (%08lx)\n", pmd_val(*pmd));
		pmd_clear(pmd);
		return;
	}
	pte = pte_offset(pmd, address);
	address &= ~PMD_MASK;
	end = address + size;
	if (end >= PMD_SIZE)
		end = PMD_SIZE;
	do {
		pte_t page = *pte;
		pte_clear(pte);
		forget_pte(page);
		address += PAGE_SIZE;
		pte++;
	} while (address < end);
}

static inline void unmap_pmd_range(pgd_t * dir, unsigned long address, unsigned long size)
{
	pmd_t * pmd;
	unsigned long end;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		printk("unmap_pmd_range: bad pgd (%08lx)\n", pgd_val(*dir));
		pgd_clear(dir);
		return;
	}
	pmd = pmd_offset(dir, address);
	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		unmap_pte_range(pmd, address, end - address);
		address = (address + PMD_SIZE) & PMD_MASK; 
		pmd++;
	} while (address < end);
}

/*
 * a more complete version of free_page_tables which performs with page
 * granularity.
 */
int unmap_page_range(unsigned long address, unsigned long size)
{
	pgd_t * dir;
	unsigned long end = address + size;

	dir = pgd_offset(current, address);
	while (address < end) {
		unmap_pmd_range(dir, address, end - address);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	}
	invalidate();
	return 0;
}

static inline void zeromap_pte_range(pte_t * pte, unsigned long address, unsigned long size, pte_t zero_pte)
{
	unsigned long end;

	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		pte_t oldpage = *pte;
		*pte = zero_pte;
		forget_pte(oldpage);
		address += PAGE_SIZE;
		pte++;
	} while (address < end);
}

static inline int zeromap_pmd_range(pmd_t * pmd, unsigned long address, unsigned long size, pte_t zero_pte)
{
	unsigned long end;

	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		pte_t * pte = pte_alloc(pmd, address);
		if (!pte)
			return -ENOMEM;
		zeromap_pte_range(pte, address, end - address, zero_pte);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
	return 0;
}
/*
	将虚拟空间中从地址address开始，长度为size的内存块所对应的物理内存都释放掉，
	同时将指向这些区域的pte都指向系统中专门开出的长度为4K，全为0的物理页。
	zeromap_page_range在现代kernel代码中没有被引用，这个函数是旧版本的Linux遗留下来的，
	在新版本中已经被zap_page_range所替代。
*/
int zeromap_page_range(unsigned long address, unsigned long size, pgprot_t prot)
{
	int error = 0;
	pgd_t * dir;
	unsigned long end = address + size;
	pte_t zero_pte;

	zero_pte = pte_wrprotect(mk_pte(ZERO_PAGE, prot));
	dir = pgd_offset(current, address);
	while (address < end) {
		pmd_t *pmd = pmd_alloc(dir, address);
		error = -ENOMEM;
		if (!pmd)
			break;
		error = zeromap_pmd_range(pmd, address, end - address, zero_pte);
		if (error)
			break;
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	}
	invalidate();
	return error;
}

/*
 * maps a range of physical memory into the requested pages. the old
 * mappings are removed. any references to nonexistent pages results
 * in null mappings (currently treated as "copy-on-access")
 */
static inline void remap_pte_range(pte_t * pte, unsigned long address, unsigned long size,
	unsigned long offset, pgprot_t prot)
{
	unsigned long end;

	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		pte_t oldpage = *pte;
		pte_clear(pte);
		if (offset >= high_memory || (mem_map[MAP_NR(offset)] & MAP_PAGE_RESERVED))
			*pte = mk_pte(offset, prot);
		else if (mem_map[MAP_NR(offset)]) {
			mem_map[MAP_NR(offset)]++;
			*pte = mk_pte(offset, prot);
		}
		forget_pte(oldpage);
		address += PAGE_SIZE;
		offset += PAGE_SIZE;
		pte++;
	} while (address < end);
}

static inline int remap_pmd_range(pmd_t * pmd, unsigned long address, unsigned long size,
	unsigned long offset, pgprot_t prot)
{
	unsigned long end;

	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	offset -= address;
	do {
		pte_t * pte = pte_alloc(pmd, address);
		if (!pte)
			return -ENOMEM;
		remap_pte_range(pte, address, end - address, address + offset, prot);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
	return 0;
}

int remap_page_range(unsigned long from, unsigned long offset, unsigned long size, pgprot_t prot)
{
	int error = 0;
	pgd_t * dir;
	unsigned long end = from + size;

	offset -= from;
	dir = pgd_offset(current, from);
	while (from < end) {
		pmd_t *pmd = pmd_alloc(dir, from);
		error = -ENOMEM;
		if (!pmd)
			break;
		error = remap_pmd_range(pmd, from, end - from, offset + from, prot);
		if (error)
			break;
		from = (from + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	}
	invalidate();
	return error;
}

/*
 * sanity-check function..
 */
static void put_page(pte_t * page_table, pte_t pte)
{
	if (!pte_none(*page_table)) {
		printk("put_page: page already exists %08lx\n", pte_val(*page_table));
		free_page(pte_page(pte));
		return;
	}
/* no need for invalidate */
	*page_table = pte;
}

/*
 * This routine is used to map in a page into an address space: needed by
 * execve() for the initial stack and environment pages.
 */
unsigned long put_dirty_page(struct task_struct * tsk, unsigned long page, unsigned long address)
{
	pgd_t * pgd;
	pmd_t * pmd;
	pte_t * pte;

	if (page >= high_memory)
		printk("put_dirty_page: trying to put page %08lx at %08lx\n",page,address);
	if (mem_map[MAP_NR(page)] != 1)
		printk("mem_map disagrees with %08lx at %08lx\n",page,address);
	pgd = pgd_offset(tsk,address);
	pmd = pmd_alloc(pgd, address);
	if (!pmd) {
		free_page(page);
		oom(tsk);
		return 0;
	}
	pte = pte_alloc(pmd, address);
	if (!pte) {
		free_page(page);
		oom(tsk);
		return 0;
	}
	if (!pte_none(*pte)) {
		printk("put_dirty_page: page already exists\n");
		pte_clear(pte);
		invalidate();
	}
	*pte = pte_mkwrite(pte_mkdirty(mk_pte(page, PAGE_COPY)));
/* no need for invalidate */
	return page;
}

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * Goto-purists beware: the only reason for goto's here is that it results
 * in better assembly code.. The "default" path will see no jumps at all.
 *
 * Note that this routine assumes that the protection checks have been
 * done by the caller (the low-level page fault routine in most cases).
 * Thus we can safely just mark it writable once we've done any necessary
 * COW.
 *
 * We also mark the page dirty at this point even though the page will
 * change only once the write actually happens. This avoids a few races,
 * and potentially makes it more efficient.
 */
void do_wp_page(struct vm_area_struct * vma, unsigned long address,
	int write_access)
{
	pgd_t *page_dir;
	pmd_t *page_middle;
	pte_t *page_table, pte;
	unsigned long old_page, new_page;

	new_page = __get_free_page(GFP_KERNEL);
	page_dir = pgd_offset(vma->vm_task,address);
	if (pgd_none(*page_dir))
		goto end_wp_page;
	if (pgd_bad(*page_dir))
		goto bad_wp_pagedir;
	page_middle = pmd_offset(page_dir, address);
	if (pmd_none(*page_middle))
		goto end_wp_page;
	if (pmd_bad(*page_middle))
		goto bad_wp_pagemiddle;
	page_table = pte_offset(page_middle, address);
	pte = *page_table;
	if (!pte_present(pte))
		goto end_wp_page;
	if (pte_write(pte))
		goto end_wp_page;
	old_page = pte_page(pte);
	if (old_page >= high_memory)
		goto bad_wp_page;
	vma->vm_task->mm->min_flt++;
	/*
	 * Do we need to copy?
	 */
	if (mem_map[MAP_NR(old_page)] != 1) {
		if (new_page) {
			if (mem_map[MAP_NR(old_page)] & MAP_PAGE_RESERVED)
				++vma->vm_task->mm->rss;
			copy_page(old_page,new_page);
			*page_table = pte_mkwrite(pte_mkdirty(mk_pte(new_page, vma->vm_page_prot)));
			free_page(old_page);	//decrementing the shared-page counter for the old page?
			invalidate();
			return;
		}
		*page_table = BAD_PAGE;
		free_page(old_page);
		oom(vma->vm_task);
		invalidate();
		return;
	}
	//说明mem_map[MAP_NR(old_page)] == 1，此页面为次进程所独享，把此页面改为可写，被修改过的，why？
	/*之所以将其设置为脏的，是为了之后将其交换缓冲删除，而且执行到这里肯定是对页面进行了写操作*/
	*page_table = pte_mkdirty(pte_mkwrite(pte));
	invalidate();
	if (new_page)
		free_page(new_page);
	return;
bad_wp_page:
	printk("do_wp_page: bogus page at address %08lx (%08lx)\n",address,old_page);
	send_sig(SIGKILL, vma->vm_task, 1);
	goto end_wp_page;
bad_wp_pagemiddle:
	printk("do_wp_page: bogus page-middle at address %08lx (%08lx)\n", address, pmd_val(*page_middle));
	send_sig(SIGKILL, vma->vm_task, 1);
	goto end_wp_page;
bad_wp_pagedir:
	printk("do_wp_page: bogus page-dir entry at address %08lx (%08lx)\n", address, pgd_val(*page_dir));
	send_sig(SIGKILL, vma->vm_task, 1);
end_wp_page:
	if (new_page)
		free_page(new_page);
	return;
}

/*
 * Ugly, ugly, but the goto's result in better assembly..
 */
int verify_area(int type, const void * addr, unsigned long size)
{
	struct vm_area_struct * vma;
	unsigned long start = (unsigned long) addr;

	/* If the current user space is mapped to kernel space (for the
	 * case where we use a fake user buffer with get_fs/set_fs()) we
	 * don't expect to find the address in the user vm map.
	 */
	if (get_fs() == get_ds())
		return 0;

	vma = find_vma(current, start);
	if (!vma)
		goto bad_area;
	if (vma->vm_start <= start)
		goto good_area;
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;
	if (vma->vm_end - start > current->rlim[RLIMIT_STACK].rlim_cur)
		goto bad_area;

good_area:
	if (type == VERIFY_WRITE)
		goto check_write;
	for (;;) {
		struct vm_area_struct * next;
		if (!(vma->vm_flags & VM_READ))
			goto bad_area;
		if (vma->vm_end - start >= size)
			return 0;
		next = vma->vm_next;
		if (!next || vma->vm_end != next->vm_start)
			goto bad_area;
		vma = next;
	}

check_write:
	if (!(vma->vm_flags & VM_WRITE))
		goto bad_area;
	if (!wp_works_ok)	/* set if paging hardware honours WP */
		goto check_wp_fault_by_hand;
	for (;;) {
		if (vma->vm_end - start >= size)
			break;
		if (!vma->vm_next || vma->vm_end != vma->vm_next->vm_start)
			goto bad_area;
		vma = vma->vm_next;
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;
	}
	return 0;

//此段代码可用来加载匿名映射的可执行文件 参加exec.c中的do_execve()及read_exec()
//load_aout_binary()
check_wp_fault_by_hand:
	size--;
	size += start & ~PAGE_MASK;
	size >>= PAGE_SHIFT;
	start &= PAGE_MASK;

	for (;;) {
		do_wp_page(vma, start, 1);
		if (!size)
			break;
		size--;
		start += PAGE_SIZE;
		if (start < vma->vm_end)
			continue;
		vma = vma->vm_next;
		if (!vma || vma->vm_start != start)
			goto bad_area;
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;;
	}
	return 0;

bad_area:
	return -EFAULT;
}

//申请一页新的页面并映射到page_table处
static inline void get_empty_page(struct vm_area_struct * vma, pte_t * page_table)
{
	unsigned long tmp;

	if (!(tmp = get_free_page(GFP_KERNEL))) {
		oom(vma->vm_task);
		put_page(page_table, BAD_PAGE);
		return;
	}
	put_page(page_table, pte_mkwrite(mk_pte(tmp, vma->vm_page_prot)));
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same inode and can generally otherwise be shared.
 */
static int try_to_share(unsigned long to_address, struct vm_area_struct * to_area,
	unsigned long from_address, struct vm_area_struct * from_area,
	unsigned long newpage)
{
	pgd_t * from_dir, * to_dir;
	pmd_t * from_middle, * to_middle;
	pte_t * from_table, * to_table;
	pte_t from, to;

	from_dir = pgd_offset(from_area->vm_task,from_address);
/* is there a page-directory at from? */
	if (pgd_none(*from_dir))
		return 0;
	if (pgd_bad(*from_dir)) {
		printk("try_to_share: bad page directory %08lx\n", pgd_val(*from_dir));
		pgd_clear(from_dir);
		return 0;
	}
	from_middle = pmd_offset(from_dir, from_address);
/* is there a mid-directory at from? */
	if (pmd_none(*from_middle))
		return 0;
	if (pmd_bad(*from_middle)) {
		printk("try_to_share: bad mid directory %08lx\n", pmd_val(*from_middle));
		pmd_clear(from_middle);
		return 0;
	}
	from_table = pte_offset(from_middle, from_address);
	from = *from_table;
/* is the page present? */
	if (!pte_present(from))
		return 0;
/* if it is dirty it must be from a shared mapping to be shared */
	if (pte_dirty(from)) {
		if (!(from_area->vm_flags & VM_SHARED))
			return 0;
		if (pte_write(from)) {	//Bug？？
			printk("nonwritable, but dirty, shared page\n");
			return 0;
		}
	}
/* is the page reasonable at all? */
	if (pte_page(from) >= high_memory)
		return 0;
	if (mem_map[MAP_NR(pte_page(from))] & MAP_PAGE_RESERVED)
		return 0;
/* is the destination ok? */
	to_dir = pgd_offset(to_area->vm_task,to_address);
/* is there a page-directory at to? */
	if (pgd_none(*to_dir))
		return 0;
	if (pgd_bad(*to_dir)) {
		printk("try_to_share: bad page directory %08lx\n", pgd_val(*to_dir));
		return 0;
	}
	to_middle = pmd_offset(to_dir, to_address);
/* is there a mid-directory at to? */
	if (pmd_none(*to_middle))
		return 0;
	if (pmd_bad(*to_middle)) {
		printk("try_to_share: bad mid directory %08lx\n", pmd_val(*to_middle));
		return 0;
	}
	to_table = pte_offset(to_middle, to_address);
	to = *to_table;
	if (!pte_none(to))
		return 0;
/* do we copy? */
	if (newpage) {
		/* if it's in the swap cache, it's dirty by implication */
		/* so we can't use it if it's not from a shared mapping */
		//参见swap.c中的try_to_swap_out函数
		if (in_swap_cache(pte_page(from))) {
			if (!(from_area->vm_flags & VM_SHARED))
				return 0;
			if (!pte_write(from)) {
				printk("nonwritable, but dirty, shared page\n");
				return 0;
			}
		}
		copy_page(pte_page(from), newpage);
		*to_table = mk_pte(newpage, to_area->vm_page_prot);
		return 1;
	}
/*
 * do a final swap-cache test before sharing them: if it's in the swap
 * cache, we have to remove it now, as we get two pointers to the same
 * physical page and the cache can't handle it. Mark the original dirty.
 *
 * NOTE! Even if "from" is dirty, "to" will be clean: if we get here
 * with a dirty "from", the from-mapping is a shared map, so we can trust
 * the page contents to be up-to-date
 */
	if (in_swap_cache(pte_page(from))) {
		if (!(from_area->vm_flags & VM_SHARED))
			return 0;
		*from_table = pte_mkdirty(from);
		delete_from_swap_cache(pte_page(from));
	}
	mem_map[MAP_NR(pte_page(from))]++;
	*to_table = mk_pte(pte_page(from), to_area->vm_page_prot);
/* Check if we need to do anything at all to the 'from' field */
	if (!pte_write(from))
		return 1;
	if (from_area->vm_flags & VM_SHARED)
		return 1;
/* ok, need to mark it read-only, so invalidate any possible old TB entry */
	*from_table = pte_wrprotect(from);
	invalidate();
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one.
 *
 * We first check if it is at all feasible by checking inode->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
static int share_page(struct vm_area_struct * area, unsigned long address,
	int write_access, unsigned long newpage)
{
	struct inode * inode;
	unsigned long offset;
	unsigned long from_address;
	unsigned long give_page;
	struct vm_area_struct * mpnt;

	if (!area || !(inode = area->vm_inode) || inode->i_count < 2)
		return 0;
	/* do we need to copy or can we just share? */
	give_page = 0;
	if (write_access && !(area->vm_flags & VM_SHARED)) {
		if (!newpage)
			return 0;
		give_page = newpage;
	}
	offset = address - area->vm_start + area->vm_offset;
	/* See if there is something in the VM we can share pages with. */
	/* Traverse the entire circular i_mmap list, except `area' itself. */
	for (mpnt = area->vm_next_share; mpnt != area; mpnt = mpnt->vm_next_share) {
		/* must be same inode */
		if (mpnt->vm_inode != inode) {
			printk("Aiee! Corrupt vm_area_struct i_mmap ring\n");
			break;	
		}
		/* offsets must be mutually page-aligned */
		if ((mpnt->vm_offset ^ area->vm_offset) & ~PAGE_MASK)
			continue;
		/* the other area must actually cover the wanted page.. */
		from_address = offset + mpnt->vm_start - mpnt->vm_offset;
		if (from_address < mpnt->vm_start || from_address >= mpnt->vm_end)
			continue;
		/* .. NOW we can actually try to use the same physical page */
		if (!try_to_share(address, area, from_address, mpnt, give_page))
			continue;
		/* free newpage if we never used it.. */
		if (give_page || !newpage)
			return 1;
		free_page(newpage);
		return 1;
	}
	return 0;
}

/*
 * fill in an empty page-table if none exists.
 */
 //取任务address虚拟地址处对应的pte项，若不存在，则填空项
static inline pte_t * get_empty_pgtable(struct task_struct * tsk,unsigned long address)
{
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(tsk, address);
	pmd = pmd_alloc(pgd, address);
	if (!pmd) {
		oom(tsk);
		return NULL;
	}
	pte = pte_alloc(pmd, address);
	if (!pte) {
		oom(tsk);
		return NULL;
	}
	return pte;
}

static inline void do_swap_page(struct vm_area_struct * vma, unsigned long address,
	pte_t * page_table, pte_t entry, int write_access)
{
	pte_t page;

	if (!vma->vm_ops || !vma->vm_ops->swapin) {
		swap_in(vma, page_table, pte_val(entry), write_access);
		return;
	}
	page = vma->vm_ops->swapin(vma, address - vma->vm_start + vma->vm_offset, pte_val(entry));
	if (pte_val(*page_table) != pte_val(entry)) {
		free_page(pte_page(page));
		return;
	}
	if (mem_map[MAP_NR(pte_page(page))] > 1 && !(vma->vm_flags & VM_SHARED))
		page = pte_wrprotect(page);
	++vma->vm_task->mm->rss;
	++vma->vm_task->mm->maj_flt;
	*page_table = page;
	return;
}

/*
 * do_no_page() tries to create a new page mapping. It aggressively
 * tries to share with existing pages, but makes a separate copy if
 * the "write_access" parameter is true in order to avoid the next
 * page fault.
 */
void do_no_page(struct vm_area_struct * vma, unsigned long address,
	int write_access)
{
	pte_t * page_table;
	pte_t entry;
	unsigned long page;

	//获取任务address处所对应的pte项
	page_table = get_empty_pgtable(vma->vm_task,address);
	if (!page_table)
		return;
	entry = *page_table;
	//如果该页的页表条目是无效的但非空，此页是在交换文件中
	if (pte_present(entry))	//如果页在主存中
		return;
	if (!pte_none(entry)) {	
		//页不在主存中，但是页表项保存了相关信息，则表明该页被内核换出，则要进行换入操作 
		do_swap_page(vma, address, page_table, entry, write_access);
		return;
	}
	//执行到这里，说明该页的页表条目是无效的并且也是空的
	//页表项内容为空，表明进程未访问过该页
	address &= PAGE_MASK;
	//通常堆栈的增长是通过下面的代码段，因为进程的堆栈vma->vm_ops为NULL
	if (!vma->vm_ops || !vma->vm_ops->nopage) {
		++vma->vm_task->mm->rss;
		++vma->vm_task->mm->min_flt;
		get_empty_page(vma, page_table);
		return;
	}
	page = get_free_page(GFP_KERNEL);
	if (share_page(vma, address, write_access, page)) {
		++vma->vm_task->mm->min_flt;
		++vma->vm_task->mm->rss;
		return;
	}
	if (!page) {
		oom(current);
		put_page(page_table, BAD_PAGE);
		return;
	}
	++vma->vm_task->mm->maj_flt;
	++vma->vm_task->mm->rss;
	/*
	 * The fourth argument is "no_share", which tells the low-level code
	 * to copy, not share the page even if sharing is possible.  It's
	 * essentially an early COW detection 
	 */
	page = vma->vm_ops->nopage(vma, address, page,
		write_access && !(vma->vm_flags & VM_SHARED));
	if (share_page(vma, address, write_access, 0)) {
		free_page(page);
		return;
	}
	/*
	 * This silly early PAGE_DIRTY setting removes a race
	 * due to the bad i386 page protection. But it's valid
	 * for other architectures too.
	 *
	 * Note that if write_access is true, we either now have
	 * a exclusive copy of the page, or this is a shared mapping,
	 * so we can make it writable and dirty to avoid having to
	 * handle that later.
	 */
	entry = mk_pte(page, vma->vm_page_prot);
	if (write_access) {
		entry = pte_mkwrite(pte_mkdirty(entry));
	} else if (mem_map[MAP_NR(page)] > 1 && !(vma->vm_flags & VM_SHARED))
		entry = pte_wrprotect(entry);
	put_page(page_table, entry);
}

/*
 * The above separate functions for the no-page and wp-page
 * cases will go away (they mostly do the same thing anyway),
 * and we'll instead use only a general "handle_mm_fault()".
 *
 * These routines also need to handle stuff like marking pages dirty
 * and/or accessed for architectures that don't do it in hardware (most
 * RISC architectures).  The early dirtying is also good on the i386.
 *
 * There is also a hook called "update_mmu_cache()" that architectures
 * with external mmu caches can use to update those (ie the Sparc or
 * PowerPC hashed page tables that act as extended TLBs).
 */
static inline void handle_pte_fault(struct vm_area_struct * vma, unsigned long address,
	int write_access, pte_t * pte)
{
	if (!pte_present(*pte)) {
		do_no_page(vma, address, write_access);
		return;
	}
	*pte = pte_mkyoung(*pte);
	if (!write_access)
		return;
	if (pte_write(*pte)) {
		*pte = pte_mkdirty(*pte);
		return;
	}
	do_wp_page(vma, address, write_access);
}

void handle_mm_fault(struct vm_area_struct * vma, unsigned long address,
	int write_access)
{
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(vma->vm_task, address);
	pmd = pmd_alloc(pgd, address);
	if (!pmd)
		goto no_memory;
	pte = pte_alloc(pmd, address);
	if (!pte)
		goto no_memory;
	handle_pte_fault(vma, address, write_access, pte);
	update_mmu_cache(vma, address, *pte);
	return;
no_memory:
	oom(vma->vm_task);
}
