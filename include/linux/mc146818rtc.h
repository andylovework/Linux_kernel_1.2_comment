/* mc146818rtc.h - register definitions for the Real-Time-Clock / CMOS RAM
 * Copyright Torsten Duwe <duwe@informatik.uni-erlangen.de> 1993
 * derived from Data Sheet, Copyright Motorola 1984 (!).
 * It was written to be part of the Linux operating system.
 */
/* permission is hereby granted to copy, modify and redistribute this code
 * in terms of the GNU Library General Public License, Version 2 or later,
 * at your option.
 */
 /*
	自从IBM PC AT起，所有的PC机就都包含了一个叫做实时时钟（RTC）的时钟芯片，
	以便在PC机断电后仍然能够继续保持时间。显然，RTC是通过主板上的电池来供电的，
	而不是通过PC机电源来供电的，因此当PC机关掉电源后，RTC仍然会继续工作。通常
	CMOS RAM和RTC被集成到一块芯片上，因此RTC也称作“CMOS Timer”。最常见的RTC芯
	片是MC146818（Motorola）和DS12887（maxim），DS12887完全兼容于MC146818，并
	有一定的扩展。
	MC146818 RTC芯片一共有64个寄存器。它们的芯片内部地址编号为0x00～0x3F（不是I/O端口地址），
	这些寄存器一共可以分为三组： 
	（1）时钟与日历寄存器组：共有10个（0x00~0x09），表示时间、日历的具体信息。
		 在PC机中，这些寄存器中的值都是以BCD格式来存储的（比如23dec＝0x23BCD）。 
	（2）状态和控制寄存器组：共有4个（0x0A~0x0D），控制RTC芯片的工作方式，并表示当前的状态。 
	（3）CMOS配置数据：通用的CMOS RAM，它们与时间无关，因此我们不关心它
	详情信息：http://linux.chinaunix.net/techdoc/system/2008/06/02/1008807.shtml
 */

#ifndef _MC146818RTC_H
#define _MC146818RTC_H
#include <asm/io.h>
//在PC机中可以通过I/O端口0x70和0x71来读写RTC芯片中的寄存器。
//其中，端口0x70是RTC的寄存器地址索引端口，0x71是数据端口。
//0x70和0x71这两个端口在asm/kernel/setup.c中通过
//request_region（）函数向系统注册 
#ifndef RTC_PORT
#define RTC_PORT(x)	(0x70 + (x))
#define RTC_ADDR(x)	(0x80 | (x))
#define RTC_ALWAYS_BCD	1
#endif

//读取CMOS的实时钟 这里通过RTC_PORT(0)宏就得到了端口0x70
#define CMOS_READ(addr) ({ \
outb_p(RTC_ADDR(addr),RTC_PORT(0)); \
inb_p(RTC_PORT(1)); \
})
#define CMOS_WRITE(val, addr) ({ \
outb_p(RTC_ADDR(addr),RTC_PORT(0)); \
outb_p(val,RTC_PORT(1)); \
})

/**********************************************************************
 * register summary
 **********************************************************************/
 /*
	时钟与日历寄存器组：共有10个（0x00~0x09），表示时间、日历的具体信息。
	在PC机中，这些寄存器中的值都是以BCD格式来存储的（比如23dec＝0x23BCD）
	
	时钟与日历寄存器组的详细解释如下： 
	Address Function 
	00 Current second for RTC 
	01 Alarm second 
	02 Current minute 
	03 Alarm minute 
	04 Current hour 
	05 Alarm hour 
	06 Current day of week（01＝Sunday） 
	07 Current date of month 
	08 Current month 
	09 Current year（final two digits，eg：93） 
 */
#define RTC_SECONDS		0
#define RTC_SECONDS_ALARM	1
#define RTC_MINUTES		2
#define RTC_MINUTES_ALARM	3
#define RTC_HOURS		4
#define RTC_HOURS_ALARM		5
/* RTC_*_alarm is always true if 2 MSBs are set */
# define RTC_ALARM_DONT_CARE 	0xC0

#define RTC_DAY_OF_WEEK		6
#define RTC_DAY_OF_MONTH	7
#define RTC_MONTH		8
#define RTC_YEAR		9

/* control registers - Moto names
 */
 //状态和控制寄存器组：共有4个（0x0A~0x0D），控制RTC芯片的工作方式，并表示当前的状态
#define RTC_REG_A		10
#define RTC_REG_B		11
#define RTC_REG_C		12
#define RTC_REG_D		13

/**********************************************************************
 * register details
 **********************************************************************/
 //以下是寄存器A的详细状态信息定义：
#define RTC_FREQ_SELECT	RTC_REG_A

/* update-in-progress  - set to "1" 244 microsecs before RTC goes off the bus,
 * reset after update (may take 1.984ms @ 32768Hz RefClock) is complete,
 * totalling to a max high interval of 2.228 ms.
 */
 /*
	Update In Progress
	当控制寄存器B中的SET标志位为0时，MC146818芯片每秒都会在芯片内部执行一个“更新周期”
	（Update Cycle），其作用是增加秒寄存器的值，并检查秒寄存器是否溢出。如果溢出，则
	增加分钟寄存器的值，如此一致下去直到年寄存器。在“更新周期”期间，时间与日期寄存器
	组（0x00~0x09）是不可用的，此时如果读取它们的值将得到未定义的值，因为MC146818在整
	个更新周期期间会把时间与日期寄存器组从CPU总线上脱离，从而防止软件程序读到一个渐变的数据。
	在MC146818的输入时钟频率（也即晶体增荡器的频率）为4.194304MHZ或1.048576MHZ的情况下
	“更新周期”需要花费248us，而对于输入时钟频率为32.768KHZ的情况，“更新周期”需要花费
	1984us＝1.984ms。控制寄存器A中的UIP标志位用来表示MC146818是否正处于更新周期中，当UIP从
	0变为1的那个时刻，就表示MC146818将在稍后马上就开更新周期。在UIP从0变到1的那个时刻与MC146818
	真正开始Update Cycle的那个时刻之间时有一段时间间隔的，通常是244us。也就是说，在UIP从0变到1的
	244us之后，时间与日期寄存器组中的值才会真正开始改变，而在这之间的244us间隔内，它们的值并
	不会真正改变
 */
//（1）bit［7］——UIP标志（Update in Progress），为1表示RTC正在更新日历寄存器组中的值，
//此时日历寄存器组是不可访问的（此时访问它们将得到一个无意义的渐变值）
# define RTC_UIP		0x80
/*
	（2）bit［6：4］——这三位是“除法器控制位”（divider-control bits），用来定义RTC的操作频率。各种可能的值如下： 
	Divider bits Time-base frequency Divider Reset Operation Mode 
	DV2 DV1 DV0 
	0 0 0 4.194304 MHZ NO YES 
	0 0 1 1.048576 MHZ NO YES 
	0 1 0 32.769 KHZ NO YES 
	1 1 0/1 任何 YES NO 
	PC机通常将Divider bits设置成“010”
*/
# define RTC_DIV_CTL		0x70
   /* divider control: refclock values 4.194 / 1.049 MHz / 32.768 kHz */
#  define RTC_REF_CLCK_4MHZ	0x00
#  define RTC_REF_CLCK_1MHZ	0x10
#  define RTC_REF_CLCK_32KHZ	0x20
   /* 2 values for divider stage reset, others for "testing purposes only" */
#  define RTC_DIV_RESET1	0x60
#  define RTC_DIV_RESET2	0x70
  /* Periodic intr. / Square wave rate select. 0=none, 1=32.8kHz,... 15=2Hz */
/*
	（3）bit［3：0］——速率选择位（Rate Selection bits），用于周期性或方波信号输出。 
	RS bits 4.194304或1.048578 MHZ 32.768 KHZ 
	RS3 RS2 RS1 RS0 周期性中断 方波 周期性中断 方波 
	0 0 0 0 None None None None 
	0 0 0 1 30.517μs 32.768 KHZ 3.90625ms 256 HZ 
	0 0 1 0 61.035μs 16.384 KHZ 
	0 0 1 1 122.070μs 8.192KHZ 
	0 1 0 0 244.141μs 4.096KHZ 
	0 1 0 1 488.281μs 2.048KHZ 
	0 1 1 0 976.562μs 1.024KHZ 
	0 1 1 1 1.953125ms 512HZ 
	1 0 0 0 3.90625ms 256HZ 
	1 0 0 1 7.8125ms 128HZ 
	1 0 1 0 15.625ms 64HZ 
	1 0 1 1 31.25ms 32HZ 
	1 1 0 0 62.5ms 16HZ 
	1 1 0 1 125ms 8HZ 
	1 1 1 0 250ms 4HZ 
	1 1 1 1 500ms 2HZ 
	PC机BIOS对其默认的设置值是“0110”
*/
# define RTC_RATE_SELECT 	0x0F

/**********************************************************************/
//以下是寄存器B的详细状态信息定义：
/*
	各位的含义如下： 
	（1）bit［7］——SET标志。为1表示RTC的所有更新过程都将终止，
		用户程序随后马上对日历寄存器组中的值进行初始化设置。为0表示将允许更新过程继续。 
	（2）bit［6］——PIE标志，周期性中断使能标志。 
	（3）bit［5］——AIE标志，告警中断使能标志。 
	（4）bit［4］——UIE标志，更新结束中断使能标志。 
	（5）bit［3］——SQWE标志，方波信号使能标志。 
	（6）bit［2］——DM标志，用来控制日历寄存器组的数据模式，0＝BCD，1＝BINARY。BIOS总是将它设置为0。 
	（7）bit［1］——24／12标志，用来控制hour寄存器，0表示12小时制，1表示24小时制。PC机BIOS总是将它设置为1。 
	（8）bit［0］——DSE标志。BIOS总是将它设置为0。
*/
#define RTC_CONTROL	RTC_REG_B
# define RTC_SET 0x80		/* disable updates for clock setting */
# define RTC_PIE 0x40		/* periodic interrupt enable */
# define RTC_AIE 0x20		/* alarm interrupt enable */
# define RTC_UIE 0x10		/* update-finished interrupt enable */
# define RTC_SQWE 0x08		/* enable square-wave output */
# define RTC_DM_BINARY 0x04	/* all time/date values are BCD if clear */
# define RTC_24H 0x02		/* 24 hour mode - else hours bit 7 means pm */
# define RTC_DST_EN 0x01	/* auto switch DST - works f. USA only */

/**********************************************************************/
//以下是寄存器C的详细状态信息定义：
/*
	状态寄存器C的格式如下： 
	（1）bit［7］——IRQF标志，中断请求标志，当该位为1时，说明寄存器B中断请求发生。 
	（2）bit［6］——PF标志，周期性中断标志，为1表示发生周期性中断请求。 
	（3）bit［5］——AF标志，告警中断标志，为1表示发生告警中断请求。 
	（4）bit［4］——UF标志，更新结束中断标志，为1表示发生更新结束中断请求。 
*/
#define RTC_INTR_FLAGS	RTC_REG_C
/* caution - cleared by read */
# define RTC_IRQF 0x80		/* any of the following 3 is active */
# define RTC_PF 0x40
# define RTC_AF 0x20
# define RTC_UF 0x10

/**********************************************************************/
//以下是寄存器D的详细状态信息定义：
/*
	状态寄存器D的格式如下： 
	（1）bit［7］——VRT标志（Valid RAM and Time），为1表示OK，为0表示RTC已经掉电。 
	（2）bit［6：0］——总是为0，未定义。
*/
#define RTC_VALID	RTC_REG_D
# define RTC_VRT 0x80		/* valid RAM and time */
/**********************************************************************/

/* example: !(CMOS_READ(RTC_CONTROL) & RTC_DM_BINARY) 
 * determines if the following two #defines are needed
 */
//将BCD码转换成二进制码
#ifndef BCD_TO_BIN
#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)
#endif

#ifndef BIN_TO_BCD
#define BIN_TO_BCD(val) ((val)=(((val)/10)<<4) + (val)%10)
#endif

#endif /* _MC146818RTC_H */
