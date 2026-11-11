/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Yabause - linkage_arm.s                                               *
 *   Copyright (C) 2009-2011 Ari64                                         *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
	/*.cpu arm9tdmi*/
       /* .fpu softvfp
	.eabi_attribute 20, 1
	.eabi_attribute 21, 1
	.eabi_attribute 23, 3
	.eabi_attribute 24, 1
	.eabi_attribute 25, 1
	.eabi_attribute 26, 2
	.eabi_attribute 30, 6
       .eabi_attribute 18, 4*/
	.file	"linkage_arm.s" 
	.global	dynarec_local
	.global	master_reg
	.global	master_cc
	.global	master_pc
	.global	master_ip
	.global	slave_reg
	.global	slave_cc
	.global	slave_pc
	.global	slave_ip
	.global	mini_ht_master
	.global	mini_ht_slave
	.global	restore_candidate
	.global	memory_map
	.global	rccount
	.bss
	/* ReST-V (ADR-222): the code buffer is sh2_dynarec.c's (not on the Vita, where it is JIT memory) */
	.align	4
	.type	dynarec_local, %object
	.size	dynarec_local, 64
dynarec_local:
	.space	64+88+12+88+12+28+256+256+512+4194304
master_reg = dynarec_local + 64
	.type	master_reg, %object
	.size	master_reg, 88
master_cc = master_reg + 88
	.type	master_cc, %object
	.size	master_cc, 4
master_pc = master_cc + 4
	.type	master_pc, %object
	.size	master_pc, 4
master_ip = master_pc + 4
	.type	master_ip, %object
	.size	master_ip, 4
slave_reg = master_ip + 4
	.type	slave_reg, %object
	.size	slave_reg, 88
slave_cc = slave_reg + 88
	.type	slave_cc, %object
	.size	slave_cc, 4
slave_pc = slave_cc + 4
	.type	slave_pc, %object
	.size	slave_pc, 4
slave_ip = slave_pc + 4
	.type	slave_ip, %object
	.size	slave_ip, 4

m68kcenticycles = slave_ip + 4
	.type	m68kcenticycles, %object
	.size	m68kcenticycles, 4
m68kcycles = m68kcenticycles + 4
	.type	m68kcycles, %object
	.size	m68kcycles, 4
decilinecount = m68kcycles + 4
	.type	decilinecount, %object
	.size	decilinecount, 4
decilinecycles = decilinecount + 4
	.type	decilinecycles, %object
	.size	decilinecycles, 4
sh2cycles = decilinecycles + 4
	.type	sh2cycles, %object
	.size	sh2cycles, 4
scucycles = sh2cycles + 4
	.type	scucycles, %object
	.size	scucycles, 4
rccount = scucycles + 4
	.type	rccount, %object
	.size	rccount, 4

mini_ht_master = rccount + 4
	.type	mini_ht_master, %object
	.size	mini_ht_master, 256
mini_ht_slave = mini_ht_master + 256
	.type	mini_ht_slave, %object
	.size	mini_ht_slave, 256
restore_candidate = mini_ht_slave + 256
	.type	restore_candidate, %object
	.size	restore_candidate, 512
memory_map = restore_candidate + 512
	.type	memory_map, %object
	.size	memory_map, 4194304

	.text
	.align	2
/* ReST-V (ADR-222): slice executor. The frame loop that was here (YabauseDynarecOneFrameExec, newline,
   cc_interrupt_master, nextline, nextframe, vblankin) ran Yabause's deciline events itself; ReST-V keeps its own frame
   loop, so the recompiled code now runs one CPU for a number of cycles and comes back to C.
   ari_exec_master(cycles) / ari_exec_slave(cycles): r10 = <cpu>_cc - cycles; the code counts r10 up and, at a block
   end where it reaches 0, calls slave_entry (master; return address = where to go on) or jumps to cc_interrupt
   (slave; r8 = where to go on). Both now store the counter and the resume address and leave. */
	.align	2
	.global	ari_exec_master
	.type	ari_exec_master, %function
ari_exec_master:
	/* reentrant (a C memory handler called from the master's code may run the slave): the caller's registers go on
	   the stack, the stack pointer of the innermost run is kept at dynarec_local+28. The code's scratch words at
	   dynarec_local+0..27 (a return address and a register kept there around C calls) are saved too: the nested
	   run's code overwrote them and the master went on at a wrong address */
	push	{r4, r5, r6, r7, r8, r9, sl, fp, lr}
	ldr	fp, =dynarec_local
	ldmia	fp, {r1, r2, r3, r4, r5, r6, r7}
	push	{r1, r2, r3, r4, r5, r6, r7, r8}
	ldr	r1, [fp, #28]
	push	{r1}
	str	sp, [fp, #28]
	str	r0, [fp, #sh2cycles-dynarec_local]
	ldr	r1, [fp, #master_cc-dynarec_local]
	sub	r10, r1, r0
	/* ReST-V (ADR-223): the host registers as the master's last slice left them (a stub resumes with some SH-2
	   registers still cached in host registers; a block entry loads its own, so this is harmless there) */
	ldr	lr, =ari_save_m
	ldmia	lr, {r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r12}
	ldr	pc, [fp, #master_ip-dynarec_local]
	.size	ari_exec_master, .-ari_exec_master

	.global	ari_exec_slave
	.type	ari_exec_slave, %function
ari_exec_slave:
	push	{r4, r5, r6, r7, r8, r9, sl, fp, lr}
	ldr	fp, =dynarec_local
	ldmia	fp, {r1, r2, r3, r4, r5, r6, r7}
	push	{r1, r2, r3, r4, r5, r6, r7, r8}
	ldr	r1, [fp, #28]
	push	{r1}
	str	sp, [fp, #28]
	str	r0, [fp, #sh2cycles-dynarec_local]
	ldr	r1, [fp, #slave_cc-dynarec_local]
	sub	r10, r1, r0
	ldr	lr, =ari_save_s
	ldmia	lr, {r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r12}
	ldr	pc, [fp, #slave_ip-dynarec_local]
	.size	ari_exec_slave, .-ari_exec_slave

	.global	slave_entry
	.type	slave_entry, %function
slave_entry:
	str	r10, [fp, #master_cc-dynarec_local]
	str	r14, [fp, #master_ip-dynarec_local]
	ldr	r14, =ari_save_m
	stmia	r14, {r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r12}
ari_exit:
	ldr	sp, [fp, #28]
	pop	{r1}
	str	r1, [fp, #28]
	pop	{r1, r2, r3, r4, r5, r6, r7, r8}
	stmia	fp, {r1, r2, r3, r4, r5, r6, r7}
	pop	{r4, r5, r6, r7, r8, r9, sl, fp, pc}
	.size	slave_entry, .-slave_entry

	.global	cc_interrupt
	.type	cc_interrupt, %function
cc_interrupt:
	str	r10, [fp, #slave_cc-dynarec_local]
	str	r8, [fp, #slave_ip-dynarec_local]
	ldr	r14, =ari_save_s
	stmia	r14, {r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r12}
	b	ari_exit
	.size	cc_interrupt, .-cc_interrupt
	.ltorg
//--------------------------------------------------------------
	.align	2
	.global	dyna_linker
	.type	dyna_linker, %function
dyna_linker:
	/* r0 = virtual target address */
	/* r1 = instruction to patch */
	mov	r6, #2048
.DYNA_LINKER_PIC:	
	ldr	r3, .DYNA_LINKER_PIC_VAL+4	
	//ldr	r3, .jiptr
	mvn	r2, #0x20000
	ldr	r7, [r1]
	sub	r6, r6, #1
	and	r2, r2, r0, lsr #12
	and	r6, r6, r0, lsr #12
	cmp	r2, #1024
	add	r12, r7, #2
	orrcs	r2, r6, #1024
	ldr	r5, [r3, r2, lsl #2]
	lsl	r12, r12, #8
	/* jump_in lookup */
.B1:
	movs	r4, r5
	beq	.B3
	ldr	r3, [r5]
	ldr	r5, [r4, #12]
	teq	r3, r0
	bne	.B1
	ldr	r4, [r4, #8]
.B2:
	mov	r5, r1
	add	r1, r1, r12, asr #6
	teq	r1, r4
	moveq	pc, r4 /* Stale i-cache */
	bl	add_link
	sub	r2, r4, r5
	and	r1, r7, #0xff000000
	lsl	r2, r2, #6
	sub	r1, r1, #2
	add	r1, r1, r2, lsr #8
	str	r1, [r5]
	mov	pc, r4
.B3:
	/* hash_table lookup */
.B3_PIC:	
	ldr	r3, .B3_PIC_VAL+4	
	//ldr	r3, .jdptr
	eor	r4, r0, r0, lsl #16
	ldr	r6, .B3_PIC_VAL+8	
//	ldr	r6, .htptr
	lsr	r4, r4, #12
	bic	r4, r4, #15
	ldr	r5, [r3, r2, lsl #2]
	ldr	r7, [r6, r4]!
	teq	r7, r0
	ldreq	pc, [r6, #4]
	ldr	r7, [r6, #8]
	teq	r7, r0
	ldreq	pc, [r6, #12]
	/* jump_dirty lookup */
.B6:
	movs	r4, r5
	beq	.B8
	ldr	r3, [r5]
	ldr	r5, [r4, #12]
	teq	r3, r0
	bne	.B6
.B7:
	ldr	r1, [r4, #8]
	/* hash_table insert */
	ldr	r2, [r6]
	ldr	r3, [r6, #4]
	str	r0, [r6]
	str	r1, [r6, #4]
	str	r2, [r6, #8]
	str	r3, [r6, #12]
	mov	pc, r1
.B8:
	mov	r4, r0
	mov	r5, r1
	bl	sh2_recompile_block
	tst	r0, r0
	mov	r0, r4
	mov	r1, r5
	beq	dyna_linker
	/* shouldn't happen */
	b	.-8
.DYNA_LINKER_PIC_VAL:
	.word 0	
	.word jump_in	
.B3_PIC_VAL:
	.word 0	
	.word jump_dirty
	.word hash_table
/*
.jiptr:
	.word	jump_in
.jdptr:
	.word	jump_dirty
.htptr:
	.word	hash_table
*/	
	.align	2
	.global	jump_vaddr_r0_master
	.type	jump_vaddr_r0_master, %function
jump_vaddr_r0_master:
	eor	r2, r0, r0, lsl #16
	b	jump_vaddr
	.size	jump_vaddr_r0_master, .-jump_vaddr_r0_master
	.global	jump_vaddr_r1_master
	.type	jump_vaddr_r1_master, %function
jump_vaddr_r1_master:
	eor	r2, r1, r1, lsl #16
	mov	r0, r1
	b	jump_vaddr
	.size	jump_vaddr_r1_master, .-jump_vaddr_r1_master
	.global	jump_vaddr_r2_master
	.type	jump_vaddr_r2_master, %function
jump_vaddr_r2_master:
	mov	r0, r2
	eor	r2, r2, r2, lsl #16
	b	jump_vaddr
	.size	jump_vaddr_r2_master, .-jump_vaddr_r2_master
	.global	jump_vaddr_r3_master
	.type	jump_vaddr_r3_master, %function
jump_vaddr_r3_master:
	eor	r2, r3, r3, lsl #16
	mov	r0, r3
	b	jump_vaddr
	.size	jump_vaddr_r3_master, .-jump_vaddr_r3_master
	.global	jump_vaddr_r4_master
	.type	jump_vaddr_r4_master, %function
jump_vaddr_r4_master:
	eor	r2, r4, r4, lsl #16
	mov	r0, r4
	b	jump_vaddr
	.size	jump_vaddr_r4_master, .-jump_vaddr_r4_master
	.global	jump_vaddr_r5_master
	.type	jump_vaddr_r5_master, %function
jump_vaddr_r5_master:
	eor	r2, r5, r5, lsl #16
	mov	r0, r5
	b	jump_vaddr
	.size	jump_vaddr_r5_master, .-jump_vaddr_r5_master
	.global	jump_vaddr_r6_master
	.type	jump_vaddr_r6_master, %function
jump_vaddr_r6_master:
	eor	r2, r6, r6, lsl #16
	mov	r0, r6
	b	jump_vaddr
	.size	jump_vaddr_r6_master, .-jump_vaddr_r6_master
	.global	jump_vaddr_r7_master
	.type	jump_vaddr_r7_master, %function
jump_vaddr_r7_master:
	eor	r2, r7, r7, lsl #16
	mov	r0, r7
	b	jump_vaddr
	.size	jump_vaddr_r7_master, .-jump_vaddr_r7_master
	.global	jump_vaddr_r8_master
	.type	jump_vaddr_r8_master, %function
jump_vaddr_r8_master:
	eor	r2, r8, r8, lsl #16
	mov	r0, r8
	b	jump_vaddr
	.size	jump_vaddr_r8_master, .-jump_vaddr_r8_master
	.global	jump_vaddr_r9_master
	.type	jump_vaddr_r9_master, %function
jump_vaddr_r9_master:
	eor	r2, r9, r9, lsl #16
	mov	r0, r9
	b	jump_vaddr
	.size	jump_vaddr_r9_master, .-jump_vaddr_r9_master
	.global	jump_vaddr_r12_master
	.type	jump_vaddr_r12_master, %function
jump_vaddr_r12_master:
	eor	r2, r12, r12, lsl #16
	mov	r0, r12
	b	jump_vaddr
	.size	jump_vaddr_r12_master, .-jump_vaddr_r12_master
	.global	jump_vaddr_r0_slave
	.type	jump_vaddr_r0_slave, %function
jump_vaddr_r0_slave:
	eor	r2, r0, r0, lsl #16
	b	jump_vaddr_slave
	.size	jump_vaddr_r0_slave, .-jump_vaddr_r0_slave
	.global	jump_vaddr_r1_slave
	.type	jump_vaddr_r1_slave, %function
jump_vaddr_r1_slave:
	eor	r2, r1, r1, lsl #16
	mov	r0, r1
	b	jump_vaddr_slave
	.size	jump_vaddr_r1_slave, .-jump_vaddr_r1_slave
	.global	jump_vaddr_r2_slave
	.type	jump_vaddr_r2_slave, %function
jump_vaddr_r2_slave:
	mov	r0, r2
	eor	r2, r2, r2, lsl #16
	b	jump_vaddr_slave
	.size	jump_vaddr_r2_slave, .-jump_vaddr_r2_slave
	.global	jump_vaddr_r3_slave
	.type	jump_vaddr_r3_slave, %function
jump_vaddr_r3_slave:
	eor	r2, r3, r3, lsl #16
	mov	r0, r3
	b	jump_vaddr_slave
	.size	jump_vaddr_r3_slave, .-jump_vaddr_r3_slave
	.global	jump_vaddr_r4_slave
	.type	jump_vaddr_r4_slave, %function
jump_vaddr_r4_slave:
	eor	r2, r4, r4, lsl #16
	mov	r0, r4
	b	jump_vaddr_slave
	.size	jump_vaddr_r4_slave, .-jump_vaddr_r4_slave
	.global	jump_vaddr_r5_slave
	.type	jump_vaddr_r5_slave, %function
jump_vaddr_r5_slave:
	eor	r2, r5, r5, lsl #16
	mov	r0, r5
	b	jump_vaddr_slave
	.size	jump_vaddr_r5_slave, .-jump_vaddr_r5_slave
	.global	jump_vaddr_r6_slave
	.type	jump_vaddr_r6_slave, %function
jump_vaddr_r6_slave:
	eor	r2, r6, r6, lsl #16
	mov	r0, r6
	b	jump_vaddr_slave
	.size	jump_vaddr_r6_slave, .-jump_vaddr_r6_slave
	.global	jump_vaddr_r7_slave
	.type	jump_vaddr_r7_slave, %function
jump_vaddr_r7_slave:
	eor	r2, r7, r7, lsl #16
	mov	r0, r7
	b	jump_vaddr_slave
	.size	jump_vaddr_r7_slave, .-jump_vaddr_r7_slave
	.global	jump_vaddr_r8_slave
	.type	jump_vaddr_r8_slave, %function
jump_vaddr_r8_slave:
	eor	r2, r8, r8, lsl #16
	mov	r0, r8
	b	jump_vaddr_slave
	.size	jump_vaddr_r8_slave, .-jump_vaddr_r8_slave
	.global	jump_vaddr_r9_slave
	.type	jump_vaddr_r9_slave, %function
jump_vaddr_r9_slave:
	eor	r2, r9, r9, lsl #16
	mov	r0, r9
	b	jump_vaddr_slave
	.size	jump_vaddr_r9_slave, .-jump_vaddr_r9_slave
	.global	jump_vaddr_r12_slave
	.type	jump_vaddr_r12_slave, %function
jump_vaddr_r12_slave:
	eor	r2, r12, r12, lsl #16
	mov	r0, r12
	b	jump_vaddr_slave
	.size	jump_vaddr_r12_slave, .-jump_vaddr_r12_slave
	.global	jump_vaddr_slave
	.type	jump_vaddr_slave, %function
jump_vaddr_slave:
	add	r2, r2, #65536
	add	r0, r0, #1
	.size	jump_vaddr_slave, .-jump_vaddr_slave
	.global	jump_vaddr
	.type	jump_vaddr, %function
jump_vaddr:
.JUMP_VADDR_PIC:	
	ldr	r1, .JUMP_VADDR_PIC_VAL+4
	//ldr	r1, .htptr
	mvn	r3, #15
	and	r2, r3, r2, lsr #12
	ldr	r2, [r1, r2]!
	teq	r2, r0
	ldreq	pc, [r1, #4]
	ldr	r2, [r1, #8]
	teq	r2, r0
	ldreq	pc, [r1, #12]
	bl	get_addr
	mov	pc, r0
.JUMP_VADDR_PIC_VAL:
	.word 0	
	.word hash_table
	.size	jump_vaddr, .-jump_vaddr
//-----------------------------------------------------------------------
	.align	2
	.global	verify_code
	.type	verify_code, %function
verify_code:
	/* r1 = source */
	/* r2 = target */
	/* r3 = length */
	tst	r3, #4
	mov	r4, #0
	add	r3, r1, r3
	mov	r5, #0
	ldrne	r4, [r1], #4
	mov	r12, #0
	ldrne	r5, [r2], #4
	teq	r1, r3
	beq	.D3
.D2:
	ldr	r7, [r1], #4
	eor	r9, r4, r5
	ldr	r8, [r2], #4
	orrs	r9, r9, r12
	bne	.D4
	ldr	r4, [r1], #4
	eor	r12, r7, r8
	ldr	r5, [r2], #4
	cmp	r1, r3
	bcc	.D2
	teq	r7, r8
.D3:
	teqeq	r4, r5
.D4:
	moveq	pc, lr
.D5:
	bl	get_addr
	mov	pc, r0
	.size	verify_code, .-verify_code
//-----------------------------------------------------------------------
	.align	2
	.global	WriteInvalidateLong
	.type	WriteInvalidateLong, %function
WriteInvalidateLong:
	/*movw	r12, #:lower16:cached_code*/
	/*movt	r12, #:upper16:cached_code*/
.WRITEINVALIDATELONG_PIC:	
	ldr	r12, .WRITEINVALIDATELONG_PIC_VAL+4
	//ldr	r12, .ccptr
	lsr	r2, r0, #17	
	lsr	r3, r0, #12
	ldr	r2, [r12, r2, lsl #2]
	mov	r12, #1
	tst	r12, r2, ror r3
	beq	ari_wl
	push	{r0, r1, r2, lr}
	bl	invalidate_addr
	pop	{r0, r1, r2, lr}
	b	ari_wl
.WRITEINVALIDATELONG_PIC_VAL:
	.word 0	
	.word cached_code	
	.size	WriteInvalidateLong, .-WriteInvalidateLong
//-----------------------------------------------------------------------	
	.align	2
	.global	WriteInvalidateWord
	.type	WriteInvalidateWord, %function
WriteInvalidateWord:
	/*movw	r12, #:lower16:cached_code*/
	/*movt	r12, #:upper16:cached_code*/
.WRITEINVALIDATEWORD_PIC:	
	ldr	r12, .WRITEINVALIDATEWORD_PIC_VAL+4
//	ldr	r12, .ccptr
	lsr	r2, r0, #17	
	lsr	r3, r0, #12
	bic	r1, r1, #0xFF000000
	ldr	r2, [r12, r2, lsl #2]
	mov	r12, #1
	/*movt	r1, #0*/
	/*uxth	r1, r1*/
	bic	r1, r1, #0xFF0000
	tst	r12, r2, ror r3
	beq	ari_ww
	push	{r0, r1, r2, lr}
	bl	invalidate_addr
	pop	{r0, r1, r2, lr}
	b	ari_ww
.WRITEINVALIDATEWORD_PIC_VAL:
	.word 0	
	.word cached_code	
	.size	WriteInvalidateWord, .-WriteInvalidateWord
//-----------------------------------------------------------------------	
	.align	2
	.global	WriteInvalidateByteSwapped
	.type	WriteInvalidateByteSwapped, %function
WriteInvalidateByteSwapped:
	eor	r0, r0, #1
	.size	WriteInvalidateByteSwapped, .-WriteInvalidateByteSwapped
//-----------------------------------------------------------------------	
	.align	2
	.global	WriteInvalidateByte
	.type	WriteInvalidateByte, %function
WriteInvalidateByte:
	/*movw	r12, #:lower16:cached_code*/
	/*movt	r12, #:upper16:cached_code*/
.WRITEINVALIDATEBYTE_PIC:	
	ldr	r12, .WRITEINVALIDATEBYTE_PIC_VAL+4
	//ldr	r12, .ccptr
	lsr	r2, r0, #17	
	lsr	r3, r0, #12
	ldr	r2, [r12, r2, lsl #2]
	mov	r12, #1
	and	r1, r1, #0xff
	tst	r12, r2, ror r3
	beq	ari_wb
	push	{r0, r1, r2, lr}
	bl	invalidate_addr
	pop	{r0, r1, r2, lr}
	b	ari_wb
.WRITEINVALIDATEBYTE_PIC_VAL:
	.word 0	
	.word cached_code		
	.size	WriteInvalidateByte, .-WriteInvalidateByte
//-----------------------------------------------------------------------
	.align	2
	.global	div1
	.type	div1, %function
div1:
	/* r0 = dividend */
	/* r1 = divisor */
	/* r2 = sr */
	tst	r2, #0x200
	bne	div1_negative_divisor
	lsrs	r2, r2, #1 /* Get T bit and shift out */
	adcs	r0, r0, r0 /* rn=(rn<<1)+T */
	adc	r3, r3, r3 /* New Q in r3 */
	mov	r4, r1     /* divisor (rm) */
	tst	r2, #0x80  /* Get (shifted) Q bit */
	mov	r5, #1
	rsbeq	r4, r1, #0 /* inverted if old_Q clear */
	moveq	r5, #0     /* 0 if old_Q clear, 1 otherwise */
	adds	r0, r4, r0 /* rn+rm if old_Q, rn-rm if !old_Q */
		           /* carry set if rn < old_rn */
	adc	r3, r5, r3 /* low bit = (rn<old_rn)^new_Q^old_Q */
	                   /* inverted for old_Q==1, ie (rn>=old_rn)^new_Q */
	orr	r2, r2, #0x80 /* set (shifted) Q bit */
	and	r3, r3, #1
	orrs	r5, r4, r5 /* zero if old_Q==0 && rn==old_rn */
	eoreq	r3, r3, #1 /* invert result for old_Q==0 && rn==old_rn */
	orr	r2, r3, r2, lsl #1 /* New T = (Q==M) */
	eor	r2, r2, r3, lsl #8 /* save new Q (=!T) */
/*
	mov	r4, r0
	mov	r5, r1
	mov	r6, r2
	mov	r7, r14
	bl	debug_division
	mov	r0, r4
	mov	r1, r5
	mov	r2, r6
	mov	r14, r7
*/
	mov	pc, lr
div1_negative_divisor:
	lsrs	r2, r2, #1 /* Get T bit and shift out */
	adcs	r0, r0, r0 /* rn=(rn<<1)+T */
	adc	r3, r3, r3 /* New Q in r3 */
	mov	r4, r1     /* divisor (rm) */
	tst	r2, #0x80  /* Get (shifted) Q bit */
	mov	r5, #1
	rsbne	r4, r1, #0 /* inverted if old_Q clear */
	movne	r5, #0     /* 0 if old_Q set, 1 otherwise */
	adds	r0, r4, r0 /* rn+rm if !old_Q, rn-rm if old_Q */
		           /* carry set if rn < old_rn */
	adc	r3, r5, r3 /* low bit = (rn<old_rn)^new_Q^old_Q */
	                   /* inverted for old_Q==1, ie (rn>=old_rn)^new_Q */
	bic	r2, r2, #0x80 /* clear (shifted) Q bit */
	and	r3, r3, #1
	orrs	r5, r4, r5 /* zero if old_Q==1 && rn==old_rn */
	/*eoreq	r3, r3, #1 /* invert result for old_Q==1 && rn==old_rn */
	eorne	r3, r3, #1 /* don't invert result for old_Q==1 && rn==old_rn */
	orr	r2, r3, r2, lsl #1 /* New T = (Q==M) */
	orr	r2, r2, r3, lsl #8 /* save new Q (=T) */
	mov	pc, lr
	.size	div1, .-div1

	.align	2
	.global	macl
	.type	macl, %function
macl:
	/* r4 = sr */
	/* r5 = multiplicand address */
	/* r6 = multiplicand address */
	/* r0 = return MACL */
	/* r1 = return MACH */
	mov	r7, r14
	mov	r8, r0 /* MACL */
	mov	r9, r1 /* MACH */
	mov	r0, r6
	bl	ari_rl
	mov	r10, r0
	mov	r0, r5
	bl	ari_rl
	add	r5, r5, #4
	add	r6, r6, #4
	mov	r12, r0
	mov	r0, r8
	mov	r1, r9
	mov	r14, r7
	smlal	r0, r1, r10, r12
	tst	r4, #2
	moveq	pc, lr
macl_saturation:
	cmn	r1, #0x8000
	mov	r7, #0x8000
	movlt	r0, #0
	rsblt	r1, r7, #0
	cmp	r1, #0x8000
	mvnge	r0, #0
	subge	r1, r7, #1
	mov	pc, lr
	.size	macl, .-macl

	.align	2
	.global	macw
	.type	macw, %function
macw:
	/* r4 = sr */
	/* r5 = multiplicand address */
	/* r6 = multiplicand address */
	/* r0 = return MACL */
	/* r1 = return MACH */
	mov	r7, r14
	mov	r8, r0 /* MACL */
	mov	r9, r1 /* MACH */
	mov	r0, r6
	bl	ari_rw
	/*sxth	r10, r0*/
	lsl	r10, r0, #16
	mov	r0, r5
	bl	ari_rw
	add	r5, r5, #2
	add	r6, r6, #2
	lsl	r12, r0, #16
	/*sxth	r12, r0*/
	mov	r0, r8
	mov	r1, r9
	asr	r10, r10, #16
	asr	r12, r12, #16
	mov	r14, r7
	smlal	r0, r1, r10, r12
	tst	r4, #2
	moveq	pc, lr
macw_saturation:
	cmn	r0, #0x80000000
	adcs	r7, r1, #0
	mvnne	r0, #0x80000000 /* 0x7FFFFFFF */
	movlt	r0, #0x80000000 /* 0x80000000 */
	mov	r1, r9
	mov	pc, lr
	.size	macw, .-macw

	.align	2
	.global	master_handle_bios
	.type	master_handle_bios, %function
master_handle_bios:
	/*movw	r1, #:lower16:MSH2*/
	str	r0, [fp, #master_pc-dynarec_local]
	/*movt	r1, #:upper16:MSH2*/
.MASTER_HANDLE_BIOS_PIC:	
	ldr	r1, .MASTER_HANDLE_BIOS_PIC_VAL+4
	//ldr	r1, .msh2ptr
	str	r10, [fp, #master_cc-dynarec_local]
	str	r14, [fp, #master_ip-dynarec_local]
	ldr	r0, [r1] /* MSH2 */
	bl	BiosHandleFunc
	ldr	r14, [fp, #master_ip-dynarec_local]
	ldr	r10, [fp, #master_cc-dynarec_local]
	mov	pc, lr
.MASTER_HANDLE_BIOS_PIC_VAL:
	.word 0
  .word MSH2
	.size	master_handle_bios, .-master_handle_bios
//----------------------------------------------------------------------
	.align	2
	.global	slave_handle_bios
	.type	slave_handle_bios, %function
slave_handle_bios:
	/*movw	r1, #:lower16:SSH2*/
	str	r0, [fp, #slave_pc-dynarec_local]
	/*movt	r1, #:upper16:SSH2*/
.SLAVE_HANDLE_BIOS_PIC:
	ldr	r1, .SLAVE_HANDLE_BIOS_PIC_VAL+4
	//ldr	r1, .ssh2ptr
	str	r10, [fp, #slave_cc-dynarec_local]
	str	r14, [fp, #slave_ip-dynarec_local]
	ldr	r0, [r1] /* SSH2 */
	bl	BiosHandleFunc
	ldr	r14, [fp, #slave_ip-dynarec_local]
	ldr	r10, [fp, #slave_cc-dynarec_local]
	mov	pc, lr
.SLAVE_HANDLE_BIOS_PIC_VAL:
	.word 0
  .word SSH2	
	.size	slave_handle_bios, .-slave_handle_bios
//----------------------------------------------------------------------
/* __clear_cache syscall for Linux OS with broken libraries */
	.align	2
	.global	clear_cache
	.type	clear_cache, %function
clear_cache:
	push	{r7, lr}
	mov	r7, #2
	mov	r2, #0  
	orr	r7, #0xf0000
	svc	0x00000000
	pop	{r7, pc}
	.size	clear_cache, .-clear_cache
//----------------------------------------------------------------------
	.align	2
	.global	breakpoint
	.type	breakpoint, %function
breakpoint:
	/* Set breakpoint here for debugging */
	mov	pc, lr
	.size	breakpoint, .-breakpoint
	.section	.note.GNU-stack,"",%progbits
