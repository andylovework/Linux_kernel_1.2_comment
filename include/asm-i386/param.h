#ifndef _ASMi386_PARAM_H
#define _ASMi386_PARAM_H
/*
	时钟滴答的频率（HZ）：也即1秒时间内PIT所产生的时钟滴答次数。类似地
	这个值也是由PIT通道0的计数器初值决定的（反过来说，确定了时钟滴答的
	频率值后也就可以确定8254 PIT通道0的计数器初值）。Linux内核用宏HZ来
	表示时钟滴答的频率，而且在不同的平台上HZ有不同的定义值。对于ALPHA和
	IA62平台HZ的值是1024，对于SPARC、MIPS、ARM和i386等平台HZ的值都是100。
	该宏在i386平台上的定义如下（include/asm-i386/param.h）： 
	#ifndef HZ 
	#define HZ 100 
	#endif 
	根据HZ的值，我们也可以知道一次时钟滴答的具体时间间隔应该是（1000ms／HZ）＝10ms。 
*/
#ifndef HZ
#define HZ 100
#endif

#define EXEC_PAGESIZE	4096

#ifndef NGROUPS
#define NGROUPS		32
#endif

#ifndef NOGROUP
#define NOGROUP		(-1)
#endif

#define MAXHOSTNAMELEN	64	/* max length of hostname */

#endif
