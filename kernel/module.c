#include <linux/errno.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <linux/mm.h>		/* defines GFP_KERNEL */
#include <linux/string.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/malloc.h>
/*
 * Originally by Anonymous (as far as I know...)
 * Linux version by Bas Laarhoven <bas@vimec.nl>
 * 0.99.14 version by Jon Tombs <jon@gtex02.us.es>,
 *
 * Heavily modified by Bjorn Ekwall <bj0rn@blox.se> May 1994 (C)
 * This source is covered by the GNU GPL, the same as all kernel sources.
 *
 * Features:
 *	- Supports stacked modules (removable only of there are no dependents).
 *	- Supports table of symbols defined by the modules.
 *	- Supports /proc/ksyms, showing value, name and owner of all
 *	  the symbols defined by all modules (in stack order).
 *	- Added module dependencies information into /proc/modules
 *	- Supports redefines of all symbols, for streams-like behaviour.
 *	- Compatible with older versions of insmod.
 *
 * New addition in December 1994: (Bjorn Ekwall, idea from Jacques Gelinas)
 *	- Externally callable function:
 *
 *		"int register_symtab(struct symbol_table *)"
 *
 *	  This function can be called from within the kernel,
 *	  and ALSO from loadable modules.
 *	  The goal is to assist in modularizing the kernel even more,
 *	  and finally: reducing the number of entries in ksyms.c
 *	  since every subsystem should now be able to decide and
 *	  control exactly what symbols it wants to export, locally!
 */
 
/*
	Linux中的可加载模块(Module)是 Linux内核支持的 动态可加载模块(loadable module)，它们是核心的一部分
	（通常是设备驱动程序），但是并没有编译到核心里面去。Module可以单独编译成为目标代码，module是一个目
	标文件。它可以根据需要在系统启动后动态地加载到系统核心之中。当module不再被需要时，可以动态地卸载出
	系统核心。Linux中大多数设备驱动程序或文件系统都作成的module。超级用户可以通过insmod和rmmod命令显示
	地将module载入核心或从核心中将它卸载。核心也可在需要时，请求守护进程(kerneld)装载和卸载module。通过
	动态地将代码载入核心可以减小核心代码的规模，使核心配置更为灵活。若在调试新核心代码时采用module技术，
	用户不必每次修改后都需重新编译核心和启动系统。

	模块的管理要能完成链接功能。这是另一种链接，它是在系统运行时将系统提供的函数和变量地址取出来，然后根
	据这些地址一一将模块目标文件中的相应位置修正过来。模块之间不是互相独立的，有些模块的实现必须依赖其他
	模块。因此，管理模块还包括记录下模块之间的引用关系。
	总结一下，模块的管理要实现：
	A.将模块动态链接进核心；
	B.记录下模块的信息和相互引用关系。
	与模块有关的数据结构主要有两个：module和symbol_table。大体来说，module这个结构用来完成上面列出的第二
	项功能，而symbol_table这个结构用来完成第一项功能。module有一个指针指向它对应的symbol_table。
*/

/*
	insmod 命令实现对理解系统装载 module 的过程十分重要。
	1.insmod 先调用系统调用 sys_get_kernel_syms,将当前加到系统中的模块和核心的符
	号表全部输出到 kernel_sym 结构中,为后面使用。这个结构的内容在 insmod 用户进程空间。
	2将 Mymodule 目标文件读进 insmod 用户进程空间,成为一个映像。
	3根据第一步得到的信息,将 Mymodule 映像中的地址没有确定的函数和变量一一修
	正过来。
	4调用系统调用 sys_create_module、sys_init_module,将 Mymodule 链入到系统中去。
	这样,insmod 就完成了将 Mymodule 加到系统中的功能。
*/

/*更详细的资料请参见 资料积累中的《Linux中的可加载模块(module)分析.doc》*/
/*在资料积累文件夹下有关于模块编程的最简单的一个例子，也可以参看我的博客
  ：http://blog.csdn.net/yihaolovem/article/details/41261755*/
/*关于利用模块截获并替换系统调用的例子见资料积累或我的博客：
  http://blog.csdn.net/yihaolovem/article/details/41288267*/

#ifdef DEBUG_MODULE
#define PRINTK(a) printk a
#else
#define PRINTK(a) /* */
#endif

/* 核心的module变量 */
static struct module kernel_module;
/* 系统中module list 的头指针，通过这个指针可以遍历所有加到系统中的module */
static struct module *module_list = &kernel_module;

/* 如果系统中有module被标记为删除，该值为true */
static int freeing_modules; /* true if some modules are marked for deletion */

static struct module *find_module( const char *name);
static int get_mod_name( char *user_name, char *buf);
static int free_modules( void);

static int module_init_flag = 0; /* Hmm... */

/*
 * Called at boot time
 */
/*
	当内核启动时，要进行很多初始化工作，其中，对模块的初始化是在main.c中调用
	init_modules()函数完成的。实际上，当内核启动时唯一的模块就为内核本身，因
	此，初始化要做的唯一工作就是求出内核符号表中符号的个数
*/
void init_modules(void) {
	extern struct symbol_table symbol_table; /* in kernel/ksyms.c 内核符号表*/
	struct internal_symbol *sym;
	int i;

	//从这里可以看出内核统计其符号表的方法是遍历符号结构，如果符号名不为空，
	//则循环会一直继续下去,直到遇到一个名字为空的符号。由此可猜想，内核符号表
	//初始化期间会将最后一个符号设置为空的一个代表结束的标记（利用/linux/symtab_end.h中的宏）
	for (i = 0, sym = symbol_table.symbol; sym->name; ++sym, ++i)
		;
	symbol_table.n_symbols = i;

	kernel_module.symtab = &symbol_table;
	kernel_module.state = MOD_RUNNING; /* Hah! */
	kernel_module.name = "";
}

//从这里可以看到，符号表存储应该具有唯一性，但是各个模块导出的的符号表应该是存储在自己模块
//中的符号表中的
int
rename_module_symbol(char *old_name, char *new_name)
{
	struct internal_symbol *sym;
	int i = 0; /* keep gcc silent */

	if (module_list->symtab) {
		sym = module_list->symtab->symbol;
		for (i = module_list->symtab->n_symbols; i > 0; ++sym, --i) {
			if (strcmp(sym->name, old_name) == 0) { /* found it! */
				sym->name = new_name; /* done! */
				PRINTK(("renamed %s to %s\n", old_name, new_name));
				return 1; /* it worked! */
			}
		}
	}
	printk("rename %s to %s failed!\n", old_name, new_name);
	return 0; /* not there... */

	/*
	 * This one will change the name of the first matching symbol!
	 *
	 * With this function, you can replace the name of a symbol defined
	 * in the current module with a new name, e.g. when you want to insert
	 * your own function instead of a previously defined function
	 * with the same name.
	 *
	 * "Normal" usage:
	 *
	 * bogus_function(int params)
	 * {
	 *	do something "smart";
	 *	return real_function(params);
	 * }
	 *
	 * ...
	 *
	 * init_module()
	 * {
	 *	if (rename_module_symbol("_bogus_function", "_real_function"))
	 *		printk("yep!\n");
	 *	else
	 *		printk("no way!\n");
	 * ...
	 * }
	 *
	 * When loading this module, real_function will be resolved
	 * to the real function address.
	 * All later loaded modules that refer to "real_function()" will
	 * then really call "bogus_function()" instead!!!
	 *
	 * This feature will give you ample opportunities to get to know
	 * the taste of your foot when you stuff it into your mouth!!!
	 */
}

/*
 * Allocate space for a module.
 */
asmlinkage unsigned long
sys_create_module(char *module_name, unsigned long size)	/*为module分配空间*/
{
	struct module *mp;
	void* addr;
	int error;
	int npages;
	int sspace = sizeof(struct module) + MOD_MAX_NAME;
	char name[MOD_MAX_NAME];

	if (!suser())
		return -EPERM;
	if (module_name == NULL || size == 0)
		return -EINVAL;
	/*将用户空间的字符串module_name拷贝到核心空间的name 中*/
	if ((error = get_mod_name(module_name, name)) != 0)
		return error;
	 /*查找名字为name 的module*/
	if (find_module(name) != NULL) {
		return -EEXIST;
	}

	/*分配空间*/
	if ((mp = (struct module*) kmalloc(sspace, GFP_KERNEL)) == NULL) {
		return -ENOMEM;
	}
	/*将name拷贝到module结构体内存块末尾(注意这里的mp+1)的第一个字节开始的地方 */
	strcpy((char *)(mp + 1), name); /* why not? */

	/*所需页面数*/
	npages = (size + sizeof (int) + 4095) / 4096;
	/*分配所需用户模块的内核空间 这里使用了vmalloc*/
	if ((addr = vmalloc(npages * 4096)) == 0) {
		kfree_s(mp, sspace);
		return -ENOMEM;
	}

	/*将module加到链表的开头*/
	mp->next = module_list;
	mp->ref = NULL;
	mp->symtab = NULL;
	mp->name = (char *)(mp + 1);
	mp->size = npages;
	//addr是模块代码所在位置
	mp->addr = addr;
	//表明模块还未初始化
	mp->state = MOD_UNINITIALIZED;
	mp->cleanup = NULL;

	/* module内存块的第一个字是module使用次数。*/
	* (int *) addr = 0;	/* set use count to zero */
	module_list = mp;	/* link it in */

	PRINTK(("module `%s' (%lu pages @ 0x%08lx) created\n",
		mp->name, (unsigned long) mp->size, (unsigned long) mp->addr));
	return (unsigned long) addr;
}

/*
 * Initialize a module.
 */
//本系统调用是初始化模块，使模块能够被系统使用
/*
	参数说明：module_name：指向模块名称
		  code：指向模块代码的启始位置
		  codesize：模块代码大小
		  routines：指向mod_routines结构，这个结构是两个函数指针（init、cleanup）
		  symtab：指向模块的符号表

	由于在调用这个系统调用之前,会调用 sys_create_module 系统调用,因此,在系统的
	模块链表中已有这个模块了。这样,本系统调用将继续 sys_create_module 没有完成的事:
	将 module 结构中各指针指向正确的地址。在这个过程中,由于模块本身在 insmod 用户进程
	空间(参看 insmod 简介),而加到系统中后就应该是系统的一部分,应该在核心空间。因
	此,必须将模块全部拷贝到核心空间,本系统调用内有几个地方调用了完成从用户空间到
	核心空间拷贝功能的函数(get_user、memcpy_fromfs)。
*/
asmlinkage int
sys_init_module(char *module_name, char *code, unsigned codesize,
		struct mod_routines *routines,
		struct symbol_table *symtab)
{
	struct module *mp;
	struct symbol_table *newtab;
	char name[MOD_MAX_NAME];
	int error;
	struct mod_routines rt;

	if (!suser())
		return -EPERM;

	/* A little bit of protection... we "know" where the user stack is... */
	if (symtab && ((unsigned long)symtab > 0xb0000000)) {
		printk("warning: you are using an old insmod, no symbols will be inserted!\n");
		symtab = NULL;
	}

	/*
	 * First reclaim any memory from dead modules that where not
	 * freed when deleted. Should I think be done by timers when
	 * the module was deleted - Jon.
	 */
	free_modules();

	//将模块名复制到内核空间name
	if ((error = get_mod_name(module_name, name)) != 0)
		return error;
	PRINTK(("initializing module `%s', %d (0x%x) bytes\n",
		name, codesize, codesize));
	//将rt初始化，其内容来自用户空间模块的rotines
	memcpy_fromfs(&rt, routines, sizeof rt);
	//由于在调用这个系统调用之前,会调用 sys_create_module 系统调用,因此,在系统的
	//模块链表中应该已有这个模块了。
	if ((mp = find_module(name)) == NULL)
		return -ENOENT;
	//如果模块的大小大于mp分配的内存大小
	if ((codesize + sizeof (int) + 4095) / 4096 > mp->size)
		return -EINVAL;
	//将模块代码复制到内核空间
	memcpy_fromfs((char *)mp->addr + sizeof (int), code, codesize);
	//对剩余的空间清零
	memset((char *)mp->addr + sizeof (int) + codesize, 0,
		mp->size * 4096 - (codesize + sizeof (int)));
	PRINTK(( "module init entry = 0x%08lx, cleanup entry = 0x%08lx\n",
		(unsigned long) rt.init, (unsigned long) rt.cleanup));
	/* 设置清除程序 */
	mp->cleanup = rt.cleanup;

	/* update kernel symbol table */
	//如果传入的符号表不为空
	if (symtab) { /* symtab == NULL means no new entries to handle */
		struct internal_symbol *sym;
		struct module_ref *ref;
		int size;
		int i;
		int legal_start;

		if ((error = verify_area(VERIFY_READ, symtab, sizeof(int))))
			return error;
		//读入符号表的大小
		memcpy_fromfs((char *)(&(size)), symtab, sizeof(int));

		//为符号表在内核空间申请内存
		if ((newtab = (struct symbol_table*) kmalloc(size, GFP_KERNEL)) == NULL) {
			return -ENOMEM;
		}

		if ((error = verify_area(VERIFY_READ, symtab, size))) {
			kfree_s(newtab, size);
			return error;
		}
		//将符号表复制到内核空间
		memcpy_fromfs((char *)(newtab), symtab, size);

		/* sanity check */
		legal_start = sizeof(struct symbol_table) +
			newtab->n_symbols * sizeof(struct internal_symbol) +
			newtab->n_refs * sizeof(struct module_ref);

		if ((newtab->n_symbols < 0) || (newtab->n_refs < 0) ||
			(legal_start > size)) {
			printk("Illegal symbol table! Rejected!\n");
			kfree_s(newtab, size);
			return -EINVAL;
		}

		/* relocate name pointers, index referred from start of table */
		for (sym = &(newtab->symbol[0]), i = 0;
			i < newtab->n_symbols; ++sym, ++i) {
			if ((unsigned long)sym->name < legal_start || size <= (unsigned long)sym->name) {
				printk("Illegal symbol table! Rejected!\n");
				kfree_s(newtab, size);
				return -EINVAL;
			}
			/* else */
			//将符号名的地址加上newtab的地址，从而将其转换成内核空间地址
			sym->name += (long)newtab;
		}
		//指向内核空间的模块符号表
		mp->symtab = newtab;

		/* Update module references.
		 * On entry, from "insmod", ref->module points to
		 * the referenced module!
		 * Now it will point to the current module instead!
		 * The ref structure becomes the first link in the linked
		 * list of references to the referenced module.
		 * Also, "sym" from above, points to the first ref entry!!!
		 */
/*
		对每一种引用信息,都从系统已有的模块链中查找对应的模块,如果没
		找到,会提示出错(通常由 kerneld 保证被引用的模块先被加载到系
		统中);如果找到,就通过指针操作将 module_ref 这一结构链接到正
		确地方,最后将结构中指向被引用模块的指针该为指向本模块。
*/
		//遍历模块符号表的module_ref数组，并正确填充信息
		for (ref = (struct module_ref *)sym, i = 0;
			i < newtab->n_refs; ++ref, ++i) {

			/* Check for valid reference */
			struct module *link = module_list;
			while (link && (ref->module != link))
				link = link->next;

			if (link == (struct module *)0) {
				printk("Non-module reference! Rejected!\n");
				return -EINVAL;
			}
			//理解module->ref的意义是关键：ref指针记录了模块之间的引用关系。
			//它指向所有引用该模块的模块，这些模块是用链表连起来的
			//之所以要这样设计的原因可能是为了在删除一个模块时判断是否有其他的模块
			//在引用本模块，若有则不可以删除
			ref->next = ref->module->ref;
			ref->module->ref = ref;
			//将结构中指向被引用模块的指针该为指向本模块
			ref->module = mp;
		}
	}

	module_init_flag = 1; /* Hmm... */
	//调用模块自身的初始化函数
	if ((*rt.init)() != 0) {
		module_init_flag = 0; /* Hmm... */
		return -EBUSY;
	}
	module_init_flag = 0; /* Hmm... */
	mp->state = MOD_RUNNING;

	return 0;
}

/*
	功能说明:由 rmmod 命令调用,将指定的 module 或由 kerneld 装入的且不在被使用的
	module 从系统模块链中取出,将它卸载出核心并释放模块占用的内存空间。
	参数说明:module_name:指向 module 名称。若该变量为 NULL,则将由 kerneld 装入
	的且不在被使用的 module 卸载出核心。若该变量不为 NULL,则将指定的 module 卸载
	出核心。
*/

asmlinkage int
sys_delete_module(char *module_name)
{
	struct module *mp;
	char name[MOD_MAX_NAME];
	int error;

	if (!suser())
		return -EPERM;
	/* else */
	/* 对非自动卸载的 module,需要提供要卸载 module 的名字 */
	if (module_name != NULL) {
		/* 将 module 的名字从用户空间 copy 到核心空间 */
		if ((error = get_mod_name(module_name, name)) != 0)
			return error;
		/* 得到 module 的指针 */
		if ((mp = find_module(name)) == NULL)
			return -ENOENT;
		/*没有依赖该 module 的 module 或该 module 的引用数为 0 ,可以将该 module 从系统中卸载*/
		if ((mp->ref != NULL) || (GET_USE_COUNT(mp) != 0))
			return -EBUSY;
		/* 若该 module 处于运行状态,调用该 module 的 cleanup()子程序 */
		if (mp->state == MOD_RUNNING)
			(*mp->cleanup)();
		/* 将该 module 的状态置为 MOD_DELETED */
		mp->state = MOD_DELETED;
	}
	/* 调用 free_modules()将该 module 从系统中卸载 */
	free_modules();
	return 0;
}


/*
 * Copy the kernel symbol table to user space.  If the argument is null,
 * just return the size of the table.
 *
 * Note that the transient module symbols are copied _first_,
 * in lifo order!!!
 *
 * The symbols to "insmod" are according to the "old" format: struct kernel_sym,
 * which is actually quite handy for this purpose.
 * Note that insmod inserts a struct symbol_table later on...
 * (as that format is quite handy for the kernel...)
 *
 * For every module, the first (pseudo)symbol copied is the module name
 * and the address of the module struct.
 * This lets "insmod" keep track of references, and build the array of
 * struct module_refs in the symbol table.
 * The format of the module name is "#module", so that "insmod" can easily
 * notice when a module name comes along. Also, this will make it possible
 * to use old versions of "insmod", albeit with reduced functionality...
 * The "kernel" module has an empty name.
 */

/*
	功能说明:由 insmod 命令调用,将系统的所有符号表全部拷贝到用户空间,供 insmod在修正
	新加入的 module 中的偏移地址时使用。 先按照 “ 先进后出 ” 的顺序拷贝 module 所提供
	的符号表
	参数说明:module_name:指向用户空间中的符号表拷贝的目的内存区。若该变量为NULL,
	返回系统中 module 提供的符号总数。
*/

asmlinkage int
sys_get_kernel_syms(struct kernel_sym *table)
{
	struct internal_symbol *from;
	struct kernel_sym isym;
	struct kernel_sym *to;
	struct module *mp = module_list;
	int i;
	int nmodsyms = 0;

	/*遍历 kernel 中的 module 链表*/
	for (mp = module_list; mp; mp = mp->next) {
		if (mp->symtab && mp->symtab->n_symbols) {
			/* include the count for the module name! */
			/* 统计系统中的符号总数量。多加的 1 是 module 名字所对应的符号*/
			nmodsyms += mp->symtab->n_symbols + 1;
		}
		else
			/* include the count for the module name! */
			nmodsyms += 1; /* return modules without symbols too */
	}

	if (table != NULL) {
		to = table;
		
		/*确认进程对用户空间有写的权限*/
		if ((i = verify_area(VERIFY_WRITE, to, nmodsyms * sizeof(*table))))
			return i;

		/* copy all module symbols first (always LIFO order) */
		/*按照 LIFO 顺序拷贝符号表 */
		for (mp = module_list; mp; mp = mp->next) {
			if (mp->state == MOD_RUNNING) {
				/* magic: write module info as a pseudo symbol */
				/*将 module 名作为第一个拷贝的符号,符号以#开头代表 module 名。*/
				isym.value = (unsigned long)mp;
				sprintf(isym.name, "#%s", mp->name);
				memcpy_tofs(to, &isym, sizeof isym);
				++to;

				/*拷贝该 module 输出的符号*/
				if (mp->symtab != NULL) {
					/*遍历 symbol 表,拷贝 symbol 的值和名字。*/
					for (i = mp->symtab->n_symbols,
						from = mp->symtab->symbol;
						i > 0; --i, ++from, ++to) {

						isym.value = (unsigned long)from->addr;
						strncpy(isym.name, from->name, sizeof isym.name);
						memcpy_tofs(to, &isym, sizeof isym);
					}
				}
			}
		}
	}

	/* 若调用参数 table 为 NULL,返回系统中 module 提供的符号总数*/
	return nmodsyms;
}


/*
 * Copy the name of a module from user space.
 */
int
get_mod_name(char *user_name, char *buf)
{
	int i;

	i = 0;
	for (i = 0 ; (buf[i] = get_fs_byte(user_name + i)) != '\0' ; ) {
		if (++i >= MOD_MAX_NAME)
			return -E2BIG;
	}
	return 0;
}


/*
 * Look for a module by name, ignoring modules marked for deletion.
 */
//寻找指定名字的模块
struct module *
find_module( const char *name)
{
	struct module *mp;

	for (mp = module_list ; mp ; mp = mp->next) {
		if (mp->state == MOD_DELETED)
			continue;
		if (!strcmp(mp->name, name))
			break;
	}
	return mp;
}

//将某模块的引用信息从引用链表中去掉
//mp 为指向 module 的指针,该 module 将要从系统中卸载。该函数将 mp 指向的 module 从
//它所依赖的所有 module 的 reference list 中脱离。
static void
drop_refs(struct module *mp)
{
	struct module *step;
	struct module_ref *prev;
	struct module_ref *ref;

	/* 遍历 module list 中的每个 module */
	/*理解这里的关键是理解模块直接的引用关系，参见《linux内核源代码情景分析》下册的模块相关章节*/
	for (step = module_list; step; step = step->next) {
		/* 遍历每个 module 的 reference list 链表 */
		for (prev = ref = step->ref; ref; ref = prev->next) {
			//mp 指向的 module 包含在当前考察的 module 的 reference list 中,
			//将 mp 指向的 module 从 reference list 中脱离开
			if (ref->module == mp) {
				if (ref == step->ref)
					step->ref = ref->next;
				else
					prev->next = ref->next;
				break; /* every module only references once! */
			}
			else
				prev = ref;
		}
	}
}

/*
 * Try to free modules which have been marked for deletion.  Returns nonzero
 * if a module was actually freed.
 */
//释放模块占用的内存空间
int
free_modules(void)
{
	struct module *mp;
	struct module **mpp;
	int did_deletion;

	did_deletion = 0;
	freeing_modules = 0;
	mpp = &module_list;
	/* 遍历系统 module list 中每个 module */
	while ((mp = *mpp) != NULL) {
		/* 只考虑标记为删除状态的 module */
		if (mp->state != MOD_DELETED) {
			mpp = &mp->next;
		} else {
			if (GET_USE_COUNT(mp) != 0) {
				freeing_modules = 1;
				mpp = &mp->next;
			} else {	/* delete it */
				*mpp = mp->next;
				/* 该 module 有 symbol table */
				if (mp->symtab) {
					/* 该 module 依赖于其它 module */
					if (mp->symtab->n_refs)
						/* 将它从它依赖的 module 的 reference list 中脱离 */
						drop_refs(mp);
					if (mp->symtab->size)
						/* 释放符号表所占用的核心空间 */
						kfree_s(mp->symtab, mp->symtab->size);
				}
				/* 释放该 module 所占用的虚拟空间 */
				vfree(mp->addr);
				/* 释放 module 数据结构占用的核心空间 */
				kfree_s(mp, sizeof(struct module) + MOD_MAX_NAME);
				did_deletion = 1;
			}
		}
	}
	return did_deletion;
}


/*
 * Called by the /proc file system to return a current list of modules.
 */
//得到当前系统中的所有模块列表
int get_module_list(char *buf)
{
	char *p;
	char *q;
	int i;
	struct module *mp;
	struct module_ref *ref;
	char size[32];

	p = buf;
	/* Do not show the kernel pseudo module */
	for (mp = module_list ; mp && mp->next; mp = mp->next) {
		if (p - buf > 4096 - 100)
			break;			/* avoid overflowing buffer */
		q = mp->name;
		i = 20;
		while (*q) {
			*p++ = *q++;
			i--;
		}
		sprintf(size, "%d", mp->size);
		i -= strlen(size);
		if (i <= 0)
			i = 1;
		while (--i >= 0)
			*p++ = ' ';
		q = size;
		while (*q)
			*p++ = *q++;
		if (mp->state == MOD_UNINITIALIZED)
			q = "  (uninitialized)";
		else if (mp->state == MOD_RUNNING)
			q = "";
		else if (mp->state == MOD_DELETED)
			q = "  (deleted)";
		else
			q = "  (bad state)";
		while (*q)
			*p++ = *q++;

		if ((ref = mp->ref) != NULL) {
			*p++ = '\t';
			*p++ = '[';
			for (; ref; ref = ref->next) {
				q = ref->module->name;
				while (*q)
					*p++ = *q++;
				if (ref->next)
					*p++ = ' ';
			}
			*p++ = ']';
		}
		*p++ = '\n';
	}
	return p - buf;
}


/*
 * Called by the /proc file system to return a current list of ksyms.
 */
//得到当前所有的符号列表
int get_ksyms_list(char *buf)
{
	struct module *mp;
	struct internal_symbol *sym;
	int i;
	char *p = buf;

	for (mp = module_list; mp; mp = mp->next) {
		if ((mp->state == MOD_RUNNING) &&
			(mp->symtab != NULL) && (mp->symtab->n_symbols > 0)) {
			for (i = mp->symtab->n_symbols,
				sym = mp->symtab->symbol;
				i > 0; --i, ++sym) {

				if (p - buf > 4096 - 100) {
					strcat(p, "...\n");
					p += strlen(p);
					return p - buf; /* avoid overflowing buffer */
				}

				if (mp->name[0]) {
					sprintf(p, "%08lx %s\t[%s]\n",
						(long)sym->addr, sym->name, mp->name);
				}
				else {
					sprintf(p, "%08lx %s\n",
						(long)sym->addr, sym->name);
				}
				p += strlen(p);
			}
		}
	}

	return p - buf;
}

/*
 * Rules:
 * - The new symbol table should be statically allocated, or else you _have_
 *   to set the "size" field of the struct to the number of bytes allocated.
 *
 * - The strings that name the symbols will not be copied, maybe the pointers
 *
 * - For a loadable module, the function should only be called in the
 *   context of init_module
 *
 * Those are the only restrictions! (apart from not being reenterable...)
 *
 * If you want to remove a symbol table for a loadable module,
 * the call looks like: "register_symtab(0)".
 *
 * The look of the code is mostly dictated by the format of
 * the frozen struct symbol_table, due to compatibility demands.
 */
#define INTSIZ sizeof(struct internal_symbol)
#define REFSIZ sizeof(struct module_ref)
#define SYMSIZ sizeof(struct symbol_table)
#define MODSIZ sizeof(struct module)
static struct symbol_table nulltab;

/*
	模块是内核功能的扩展。用户态的进程是调用libc库，然后实现系统调用的。在编译用户
	态进程时可以使用静态库，也可以使用动态库。模块是内核的一部分，无法调用用户态的
	库，只能调用内核自己实现的一些调用。动态库无法用于模块，只用于用户态进程。

	内核符号表
	Kernel symbol table，内核符号表。
	Linux内核的符号表位于两个部分：
	静态的符号表，即内核映像vmlinuz的符号表（System.map）
	动态的符号表，即内核模块的符号表（/proc/kallsyms）

	Linux内核为了实现模块化，需要提供一个公共的内核符号表，它包含了所有的全局内核项
	（函数以及变量）的地址。当模块加载到内核中后，它所导出的任何符号都将成为内核公共
	符号表的一部分。内核模块只需要实现自己的功能而无需导出任何符号，但这样其他模块将
	无法使用该模块的功能，一个新的模块可以使用其他模块导出的符号，这样可以实现在
	其他模块的基础上层叠新的模块，如msdos文件系统依赖于由fat模块导出的符号，USB输入
	设备模块会层叠在usbcore和input模块之上。模块层叠技术在复杂项目中非常有用，如果以
	设备驱动程序的形式实现一个新的软件抽象，则可以为硬件相关的实现提供一个“插头”。
	modprobe是处理层叠模块的一个非常实用的工具，它在装载指定模块的同时也会加载该模块
	所以来的其他模块。因此一个modprobe命令有的时候相当于执行了多次insmod命令。

	一种开放你的模块中的全局符号的方法是使用函数register_symtab，这个函数是符号表管理
	的正式接口。正如函数register_symtab的名字所暗示，它用来在内核主符号表中注册符号表。
	这种方法要比通过静态和全局变量的方法清晰的多，这样程序员就可以把关于哪些开放给其他
	模块，哪些不开放的信息集中存放。这种方法比在源文件中到处堆放static声明要好的多。
	如果模块在初始化过程中调用了register_symtab，全局变量就不再是开放的了；只有那些显
	式罗列在符号表中的符号才开放给内核。由于register_symtab是在模块加载到内核后被调用的，
	它可以覆盖模块静态或全局声明的符号。此时，register_symtab用显式符号表替代模块默认开
	放的公共符号。
*/

//登记符号表 实则是将符号表放在了代表自己的那个模块的符号表中，而不是真正意义上的“内核符号表”
int
register_symtab(struct symbol_table *intab)
{
	struct module *mp;
	struct module *link;
	struct symbol_table *oldtab;
	struct symbol_table *newtab;
	struct module_ref *newref;
	int size;

	if (intab && (intab->n_symbols == 0)) {
		struct internal_symbol *sym;
		/* How many symbols, really? */
		//统计符号数
		for (sym = intab->symbol; sym->name; ++sym)
			intab->n_symbols +=1;
	}

#if 1
	if (module_init_flag == 0) { /* Hmm... */
#else
	//从内核内部调用
	if (module_list == &kernel_module) {
#endif
		/* Aha! Called from an "internal" module */
		if (!intab)
			return 0; /* or -ESILLY_PROGRAMMER :-) */

		/* create a pseudo module! */
		//分配一个新的、虚拟的模块结构
		if (!(mp = (struct module*) kmalloc(MODSIZ, GFP_KERNEL))) {
			/* panic time! */
			printk("Out of memory for new symbol table!\n");
			return -ENOMEM;
		}
		/* else  OK */
		//清零
		memset(mp, 0, MODSIZ);
		mp->state = MOD_RUNNING; /* Since it is resident... */
		mp->name = ""; /* This is still the "kernel" symbol table! */
		//将符号表链入此模块结构中
		mp->symtab = intab;

		/* link it in _after_ the resident symbol table */
		//将此模块链入系统模块链表
		mp->next = kernel_module.next;
		kernel_module.next = mp;

		return 0;
	}

	/* else ******** Called from a loadable module **********/
	//否则，就是从可加载模块调用的

	/*
	 * This call should _only_ be done in the context of the
	 * call to  init_module  i.e. when loading the module!!
	 * Or else...
	 */
        //这里要注意的是，此系统调用应该在模块初始化的时候调用，否则，
	//可能替换掉其他模块的符号表，引起内核崩溃，所以，这里也体现
	//了模块是内核的一部分，内核给予模块代码充分的信任
	mp = module_list; /* true when doing init_module! */

	/* Any table there before? */
	//如果当前模块之前不存在符号表，则直接将intab加入此模块就可以了
	if ((oldtab = mp->symtab) == (struct symbol_table*)0) {
		/* No, just insert it! */
		mp->symtab = intab;
		return 0;
	}

	/* else  ****** we have to replace the module symbol table ******/
	//否则，我们不得不替换掉当前模块的符号表
#if 0
	if (oldtab->n_symbols > 0) {
		/* Oh dear, I have to drop the old ones... */
		printk("Warning, dropping old symbols\n");
	}
#endif
	//执行到这里，说明当前模块的符号表不为空

	//如果当前模块的引用模块数为0,则可以直接替换掉旧的符号表
	if (oldtab->n_refs == 0) { /* no problems! */
		//指向新的符号表
		mp->symtab = intab;
		/* if the old table was kmalloc-ed, drop it */
		//释放掉旧的符号表
		if (oldtab->size > 0)
			kfree_s(oldtab, oldtab->size);

		return 0;
	}

	/* else */
	//执行到这里，说明当前模块引用了其他的模块
	/***** The module references other modules... insmod said so! *****/
	/* We have to allocate a new symbol table, or we lose them! */
	//我们不得不分配一个新的符号表，否则我们会丢失这些引用信息
	if (intab == (struct symbol_table*)0)
		intab = &nulltab; /* easier code with zeroes in place */

	/* the input symbol table space does not include the string table */
	/* (it does for symbol tables that insmod creates) */

	//分配新的符号表，包含了符号表数组的大小和引用模块所需指针个数的大小等信息
	if (!(newtab = (struct symbol_table*)kmalloc(
		size = SYMSIZ + intab->n_symbols * INTSIZ +
			oldtab->n_refs * REFSIZ,
		GFP_KERNEL))) {
		/* panic time! */
		printk("Out of memory for new symbol table!\n");
		return -ENOMEM;
	}

	/* copy up to, and including, the new symbols */
	//复制符号表信息
	memcpy(newtab, intab, SYMSIZ + intab->n_symbols * INTSIZ);

	newtab->size = size;
	//引用个数
	newtab->n_refs = oldtab->n_refs;

	/* copy references */
	//复制引用信息
	memcpy( ((char *)newtab) + SYMSIZ + intab->n_symbols * INTSIZ,
		((char *)oldtab) + SYMSIZ + oldtab->n_symbols * INTSIZ,
		oldtab->n_refs * REFSIZ);

	/* relink references from the old table to the new one */

	/* pointer to the first reference entry in newtab! Really! */
	//指向第一个引用指针
	newref = (struct module_ref*) &(newtab->symbol[newtab->n_symbols]);

	/* check for reference links from previous modules */
	//检验引用指针形成的链表
	for (	link = module_list;
		link && (link != &kernel_module);
		link = link->next) {

		if (link->ref->module == mp)
			link->ref = newref++;
	}

	//模块指向新的符号表
	mp->symtab = newtab;

	/* all references (if any) have been handled */

	/* if the old table was kmalloc-ed, drop it */
	//释放旧的符号表
	if (oldtab->size > 0)
		kfree_s(oldtab, oldtab->size);

	return 0;
}
