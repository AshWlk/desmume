@ ds2sf test ROM, ARM9 side.
@ Sends one command word to the ARM7 over the IPC FIFO, then idles. The mini2sf fixture patches
@ `command` (through the 2SF lib chain) to change which tone the ARM7 plays.

	.syntax unified
	.arm
	.global _start

_start:
	ldr	r0, =0x04000184		@ IPCFIFOCNT
	ldr	r1, =0x8008		@ enable FIFO, clear send FIFO
	strh	r1, [r0]

	ldr	r0, =0x04000188		@ IPCFIFOSEND
	ldr	r1, command
	str	r1, [r0]

idle:
	b	idle

	.balign 4
command:
	.word	0x440

	.ltorg
