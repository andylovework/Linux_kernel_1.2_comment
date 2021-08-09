#ifndef _LINUX_TTY_FLIP_H
#define _LINUX_TTY_FLIP_H

#ifdef INCLUDE_INLINE_FUNCS
#define _INLINE_ extern
#else
#define _INLINE_ extern __inline__
#endif

_INLINE_ void tty_insert_flip_char(struct tty_struct *tty,
				   unsigned char ch, char flag)
{
	//若发现缓冲区已满，就把接收到的字符丢弃掉
	if (tty->flip.count++ >= TTY_FLIPBUF_SIZE)
		return;
	*tty->flip.flag_buf_ptr++ = flag;
	*tty->flip.char_buf_ptr++ = ch;
}

_INLINE_ void tty_schedule_flip(struct tty_struct *tty)
{
	queue_task(&tty->flip.tqueue, &tq_timer);
}

#undef _INLINE_


#endif /* _LINUX_TTY_FLIP_H */







