#ifndef IO_H
#define	IO_H

/* 
 * File:   io.h
 * Author: http://git.musl-libc.org/cgit/musl/tree/arch/i386/bits/io.h
 *
 * Created on April 27, 2016, 3:54 PM
 */


#define PIC1_CMD                    0x20
#define PIC1_DATA                   0x21
#define PIC2_CMD                    0xA0
#define PIC2_DATA                   0xA1
#define PIC_READ_IRR                0x0a    /* OCW3 irq ready next CMD read */
#define PIC_READ_ISR                0x0b    /* OCW3 irq service next CMD read */


static __inline void outb(unsigned short __port, unsigned char __val)
{
	__asm__ volatile ("outb %1, %0" : : "a" (__val), "dN" (__port));
}

static __inline void outw(unsigned short __port, unsigned short __val)
{
	__asm__ volatile ("outw %1, %0" : : "a" (__val), "dN" (__port));
}

static __inline void outl(unsigned short __port, unsigned int __val)
{
	__asm__ volatile ("outd %1, %0" : : "a" (__val), "dN" (__port));
}

static __inline unsigned char inb(unsigned short __port)
{
	unsigned char __val;
	__asm__ volatile ("inb %0, %1" : "=a" (__val) : "dN" (__port));
	return __val;
}

static __inline unsigned short inw(unsigned short __port)
{
	unsigned short __val;
	__asm__ volatile ("inw %0, %1" : "=a" (__val) : "dN" (__port));
	return __val;
}

static __inline unsigned int inl(unsigned short __port)
{
	unsigned int __val;
	__asm__ volatile ("ind %0, %1" : "=a" (__val) : "dN" (__port));
	return __val;
}

static __inline void outsb(unsigned short __port, const void *__buf, unsigned long __n)
{
	__asm__ volatile ("cld; rep; outsb"
		      : "+S" (__buf), "+c" (__n)
		      : "d" (__port));
}

static __inline void outsw(unsigned short __port, const void *__buf, unsigned long __n)
{
	__asm__ volatile ("cld; rep; outsw"
		      : "+S" (__buf), "+c" (__n)
		      : "d" (__port));
}

static __inline void outsl(unsigned short __port, const void *__buf, unsigned long __n)
{
	__asm__ volatile ("cld; rep; outsd"
		      : "+S" (__buf), "+c"(__n)
		      : "d" (__port));
}

static __inline void insb(unsigned short __port, void *__buf, unsigned long __n)
{
	__asm__ volatile ("cld; rep; insb"
		      : "+D" (__buf), "+c" (__n)
		      : "d" (__port));
}

static __inline void insw(unsigned short __port, void *__buf, unsigned long __n)
{
	__asm__ volatile ("cld; rep; insw"
		      : "+D" (__buf), "+c" (__n)
		      : "d" (__port));
}

static __inline void insl(unsigned short __port, void *__buf, unsigned long __n)
{
	__asm__ volatile ("cld; rep; insd"
		      : "+D" (__buf), "+c" (__n)
		      : "d" (__port));
}
#endif	/* IO_H */

