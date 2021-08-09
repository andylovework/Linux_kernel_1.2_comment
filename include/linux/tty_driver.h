#ifndef _LINUX_TTY_DRIVER_H
#define _LINUX_TTY_DRIVER_H

/*
 * This structure defines the interface between the low-level tty
 * driver and the tty routines.  The following routines can be
 * defined; unless noted otherwise, they are optional, and can be
 * filled in with a null pointer.
 *
 * int  (*open)(struct tty_struct * tty, struct file * filp);
 *
 * 	This routine is called when a particular tty device is opened.
 * 	This routine is mandatory; if this routine is not filled in,
 * 	the attempted open will fail with ENODEV.
 *     
 * void (*close)(struct tty_struct * tty, struct file * filp);
 *
 * 	This routine is called when a particular tty device is closed.
 *
 * int (*write)(struct tty_struct * tty, int from_user,
 * 		 unsigned char *buf, int count);
 *
 * 	This routine is called by the kernel to write a series of
 * 	characters to the tty device.  The characters may come from
 * 	user space or kernel space.  This routine will return the
 *	number of characters actually accepted for writing.  This
 *	routine is mandatory.
 *
 * void (*put_char)(struct tty_struct *tty, unsigned char ch);
 *
 * 	This routine is called by the kernel to write a single
 * 	character to the tty device.  If the kernel uses this routine,
 * 	it must call the flush_chars() routine (if defined) when it is
 * 	done stuffing characters into the driver.  If there is no room
 * 	in the queue, the character is ignored.
 *
 * void (*flush_chars)(struct tty_struct *tty);
 *
 * 	This routine is called by the kernel after it has written a
 * 	series of characters to the tty device using put_char().  
 * 
 * int  (*write_room)(struct tty_struct *tty);
 *
 * 	This routine returns the numbers of characters the tty driver
 * 	will accept for queuing to be written.  This number is subject
 * 	to change as output buffers get emptied, or if the output flow
 *	control is acted.
 * 
 * int  (*ioctl)(struct tty_struct *tty, struct file * file,
 * 	    unsigned int cmd, unsigned long arg);
 *
 * 	This routine allows the tty driver to implement
 *	device-specific ioctl's.  If the ioctl number passed in cmd
 * 	is not recognized by the driver, it should return ENOIOCTLCMD.
 * 
 * void (*set_termios)(struct tty_struct *tty, struct termios * old);
 *
 * 	This routine allows the tty driver to be notified when
 * 	device's termios settings have changed.  Note that a
 * 	well-designed tty driver should be prepared to accept the case
 * 	where old == NULL, and try to do something rational.
 *
 * void (*set_ldisc)(struct tty_struct *tty);
 *
 * 	This routine allows the tty driver to be notified when the
 * 	device's termios settings have changed.
 * 
 * void (*throttle)(struct tty_struct * tty);
 *
 * 	This routine notifies the tty driver that input buffers for
 * 	the line discipline are close to full, and it should somehow
 * 	signal that no more characters should be sent to the tty.
 * 
 * void (*unthrottle)(struct tty_struct * tty);
 *
 * 	This routine notifies the tty drivers that it should signals
 * 	that characters can now be sent to the tty without fear of
 * 	overrunning the input buffers of the line disciplines.
 * 
 * void (*stop)(struct tty_struct *tty);
 *
 * 	This routine notifies the tty driver that it should stop
 * 	outputting characters to the tty device.  
 * 
 * void (*start)(struct tty_struct *tty);
 *
 * 	This routine notifies the tty driver that it resume sending
 *	characters to the tty device.
 * 
 * void (*hangup)(struct tty_struct *tty);
 *
 * 	This routine notifies the tty driver that it should hangup the
 * 	tty device.
 * 
 */

#include <linux/fs.h>

/*
	结构中给定了该种终端设备的主设备号以及次设备号的范围，并提供了很多函数指针
	这样，对于不同种类的终端设备就有不同的tty_driver数据结构，例如控制台终端
	的tty_driver数据结构就是console_driver
	这些函数指针确定了对具体类型的终端的一整套操作
*/
//任何一个 tty 驱动的主要数据结构是 struct tty_driver.它用来注册和注销一个 tty驱动到 tty内核
struct tty_driver {
	int	magic;		/* magic number for this structure */
	char	*name;
	int	name_base;	/* offset of printed name */
	short	major;		/* major device number */
	short	minor_start;	/* start of minor device number*/
	short	num;		/* number of devices */
	short	type;		/* type of tty driver */
	short	subtype;	/* subtype of tty driver */
	struct termios init_termios; /* Initial termios */
	int	flags;		/* tty driver flags */
	int	*refcount;	/* for loadable tty drivers */
	struct tty_driver *other; /* only used for the PTY driver */

	/*
	 * Pointer to the tty data structures
	 */
	struct tty_struct **table;
	struct termios **termios;
	struct termios **termios_locked;
	
	/*
	 * Interface routines from the upper tty layer to the tty
	 * driver.
	 */
	int  (*open)(struct tty_struct * tty, struct file * filp);
	void (*close)(struct tty_struct * tty, struct file * filp);
	int  (*write)(struct tty_struct * tty, int from_user,
		      unsigned char *buf, int count);
	void (*put_char)(struct tty_struct *tty, unsigned char ch);
	//用于刷新数据到硬件
	void (*flush_chars)(struct tty_struct *tty);
	//指示有多少缓冲区空闲
	int  (*write_room)(struct tty_struct *tty);
	//指示缓冲中包含的数据数
	int  (*chars_in_buffer)(struct tty_struct *tty);
	int  (*ioctl)(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg);
	//用于改变termios设置
	void (*set_termios)(struct tty_struct *tty, struct termios * old);
	//throttle(),unthrottle()，当tty核心的输入缓冲区满时，throttle()函数将被调用，
	//tty驱动试图通知设备不应当发送字符给它。当输入缓冲区清空，unthrottle()调用。
	void (*throttle)(struct tty_struct * tty);
	void (*unthrottle)(struct tty_struct * tty);
	//stop(),start()，停止和恢复数据发送
	void (*stop)(struct tty_struct *tty);
	void (*start)(struct tty_struct *tty);
	//挂起tty设备
	void (*hangup)(struct tty_struct *tty);
	//刷新缓冲区并丢弃任何剩下的数据。
	void (*flush_buffer)(struct tty_struct *tty);
	//设置线路规程（通常不需要被实现）
	void (*set_ldisc)(struct tty_struct *tty);

	/*
	 * linked list pointers
	 */
	struct tty_driver *next;
	struct tty_driver *prev;
};

/* tty driver magic number */
#define TTY_DRIVER_MAGIC		0x5402

/*
 * tty driver flags
 * 
 * TTY_DRIVER_RESET_TERMIOS --- requests the tty layer to reset the
 * 	termios setting when the last process has closed the device.
 * 	Used for PTY's, in particular.
 * 
 * TTY_DRIVER_REAL_RAW --- if set, indicates that the driver will
 * 	guarantee never not to set any special character handling
 * 	flags if ((IGNBRK || (!BRKINT && !PARMRK)) && (IGNPAR ||
 * 	!INPCK)).  That is, if there is no reason for the driver to
 * 	send notifications of parity and break characters up to the
 * 	line driver, it won't do so.  This allows the line driver to
 *	optimize for this case if this flag is set.  (Note that there
 * 	is also a promise, if the above case is true, not to signal
 * 	overruns, either.)
 */
#define TTY_DRIVER_INSTALLED		0x0001
#define TTY_DRIVER_RESET_TERMIOS	0x0002
#define TTY_DRIVER_REAL_RAW		0x0004

/* tty driver types */
#define TTY_DRIVER_TYPE_SYSTEM		0x0001
#define TTY_DRIVER_TYPE_CONSOLE		0x0002
#define TTY_DRIVER_TYPE_SERIAL		0x0003
#define TTY_DRIVER_TYPE_PTY		0x0004

/* system subtypes (magic, used by tty_io.c) */
#define SYSTEM_TYPE_TTY			0x0001
#define SYSTEM_TYPE_CONSOLE		0x0002

/* pty subtypes (magic, used by tty_io.c) */
#define PTY_TYPE_MASTER			0x0001
#define PTY_TYPE_SLAVE			0x0002

#endif /* #ifdef _LINUX_TTY_DRIVER_H */
