
kernel/bin/test_elf:     file format elf64-x86-64


Disassembly of section .text:

0000000000400000 <_start>:
_start():
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:6
.intel_syntax noprefix
.section .text
.global _start

_start:
    mov dx, 0x3f8
  400000:	66 ba f8 03          	mov    dx,0x3f8
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:7
    mov al, 'E'
  400004:	b0 45                	mov    al,0x45
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:8
    out dx, al
  400006:	ee                   	out    dx,al
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:9
    mov al, 'L'
  400007:	b0 4c                	mov    al,0x4c
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:10
    out dx, al
  400009:	ee                   	out    dx,al
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:11
    mov al, 'F'
  40000a:	b0 46                	mov    al,0x46
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:12
    out dx, al
  40000c:	ee                   	out    dx,al
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:13
    mov al, ' '
  40000d:	b0 20                	mov    al,0x20
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:14
    out dx, al
  40000f:	ee                   	out    dx,al
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:15
    mov al, 'O'
  400010:	b0 4f                	mov    al,0x4f
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:16
    out dx, al
  400012:	ee                   	out    dx,al
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:17
    mov al, 'K'
  400013:	b0 4b                	mov    al,0x4b
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:18
    out dx, al
  400015:	ee                   	out    dx,al
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:19
    mov al, '\n'
  400016:	b0 0a                	mov    al,0xa
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:20
    out dx, al
  400018:	ee                   	out    dx,al
/home/yogi/src/os64/kernel/test/elf/serial_ping.S:22

    ret
  400019:	c3                   	ret
