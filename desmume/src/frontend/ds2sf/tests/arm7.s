@ ds2sf test ROM, ARM7 side.
@ Waits for a command word from the ARM9 on the IPC FIFO, then starts PSG channel 8 as a 50% duty
@ square wave, centre-panned at full volume:
@   command 0x440 -> timer 0xED67 -> 16756991 / (0x10000 - 0xED67) / 8 = 439.95 Hz
@   command 0x880 -> timer 0xF6B4 -> 16756991 / (0x10000 - 0xF6B4) / 8 = 880.09 Hz
@ (PSG channels step through an 8-sample duty pattern at ARM7_CLOCK/2 / (0x10000 - timer) Hz.)

	.syntax unified
	.arm
	.global _start

_start:
	ldr	r0, =0x04000184		@ IPCFIFOCNT
	ldr	r1, =0x8008		@ enable FIFO, clear send FIFO
	strh	r1, [r0]

wait:
	ldrh	r2, [r0]
	tst	r2, #0x100		@ receive FIFO empty?
	bne	wait

	ldr	r3, =0x04100000		@ IPCFIFORECV
	ldr	r4, [r3]

	ldr	r0, =0x04000304		@ POWCNT2: sound speakers on
	mov	r1, #1
	strh	r1, [r0]

	ldr	r0, =0x04000500		@ SOUNDCNT: master enable, master volume 0x7F
	ldr	r1, =0x807F
	strh	r1, [r0]

	ldr	r0, =0x04000504		@ SOUNDBIAS
	mov	r1, #0x200
	strh	r1, [r0]

	ldr	r5, =0xED67		@ 440 Hz
	ldr	r6, =0x880
	cmp	r4, r6
	ldreq	r5, =0xF6B4		@ 880 Hz

	ldr	r0, =0x04000488		@ SOUND8TMR
	strh	r5, [r0]

	ldr	r0, =0x04000480		@ SOUND8CNT
	ldr	r1, =0xEB40007F		@ start | PSG | loop | duty 4/8 | pan 0x40 | volume 0x7F
	str	r1, [r0]

idle:
	b	idle

	.ltorg
