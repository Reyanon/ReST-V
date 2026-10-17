/*
  ReST-V: fast memory helpers for the devmiyax ARM dynarec (ADR-017).
  The block templates call the getmem and setmem pointers through function pointers (r0 = address, r1 = value).
  These handle the cached high work RAM region 0x06000000-0x060FFFFF directly, send on-chip module
  addresses (0xFFFFFE00-) straight to Onchip* (as MappedMemory* does), and tail-call the
  original C functions (memGetX and memSetX in DynarecSh2CInterface.cpp) for everything else.
  Stores also go to the C function when the 32-byte chunk holds compiled code (rstv_code_chunk),
  so self-modifying code is still caught by setDirty exactly as before.
  HighWram layout is T2 (16-bit native words): long = word-swapped, byte = address ^ 1.
  Only r0-r3 and r12 are used (caller-saved), like the C functions.
*/
.syntax unified
.arm
.text
.align 4

/* not high work RAM: on-chip modules (0xFFFFFE00-0xFFFFFFFF) go straight to Onchip*, the rest to the C function */
.macro HIGH_CHECK slow onchip
  lsr r12, r0, #20
  cmp r12, #0x060
  beq 1f
  cmn r0, #0x200
  bcc \slow
  bfc r0, #9, #23
  b \onchip
1:
.endm

.global rstv_memGetLong
.type rstv_memGetLong, %function
rstv_memGetLong:
  HIGH_CHECK memGetLong, OnchipReadLong
  ldr r12, =HighWram
  ldr r12, [r12]
  bfc r0, #20, #12
  ldr r0, [r12, r0]
  ror r0, r0, #16
  bx lr

.global rstv_memGetWord
.type rstv_memGetWord, %function
rstv_memGetWord:
  HIGH_CHECK memGetWord, OnchipReadWord
  ldr r12, =HighWram
  ldr r12, [r12]
  bfc r0, #20, #12
  ldrh r0, [r12, r0]
  bx lr

.global rstv_memGetByte
.type rstv_memGetByte, %function
rstv_memGetByte:
  HIGH_CHECK memGetByte, OnchipReadByte
  ldr r12, =HighWram
  ldr r12, [r12]
  bfc r0, #20, #12
  eor r0, r0, #1
  ldrb r0, [r12, r0]
  bx lr

.macro CODE_CHECK slow
  ldr r12, =rstv_code_chunk
  ubfx r2, r0, #5, #15
  ldrb r2, [r12, r2]
  cmp r2, #0
  bne \slow
.endm

.global rstv_memSetLong
.type rstv_memSetLong, %function
rstv_memSetLong:
  HIGH_CHECK memSetLong, OnchipWriteLong
  CODE_CHECK memSetLong
  ldr r12, =HighWram
  ldr r12, [r12]
  bfc r0, #20, #12
  ror r1, r1, #16
  str r1, [r12, r0]
  bx lr

.global rstv_memSetWord
.type rstv_memSetWord, %function
rstv_memSetWord:
  HIGH_CHECK memSetWord, OnchipWriteWord
  CODE_CHECK memSetWord
  ldr r12, =HighWram
  ldr r12, [r12]
  bfc r0, #20, #12
  strh r1, [r12, r0]
  bx lr

.global rstv_memSetByte
.type rstv_memSetByte, %function
rstv_memSetByte:
  HIGH_CHECK memSetByte, OnchipWriteByte
  CODE_CHECK memSetByte
  ldr r12, =HighWram
  ldr r12, [r12]
  bfc r0, #20, #12
  eor r0, r0, #1
  strb r1, [r12, r0]
  bx lr

.ltorg

/*
  ReST-V block chaining (ADR-018). Block exits (epilogue, PageFlip, delay-slot exits) jump here through
  tagSH2.chain with PC and COUNT already stored, r7 = state, r9 = count, the prologue's frame on the stack.
  If the next block can run right away (cycles left, no pending interrupt, last block not a loop block,
  next PC in high work RAM, compiled, not an idle-loop head) jump past its prologue; else return to Execute.
*/
.global rstv_chain
.type rstv_chain, %function
rstv_chain:
  ldr r0, [r7, #136]          @ memory wait cycles of the C memory functions
  ldr r1, [r0]
  cmp r1, #0
  beq 1f
  add r9, r9, r1
  mov r1, #0
  str r1, [r0]
  str r9, [r7, #((16+3+4)*4)]
1:
  ldr r2, [r7, #128]          @ exitcount
  cmp r9, r2
  bhs 9f
  ldr r0, [r7, #(16*4)]       @ SR
  and r0, r0, #0xF0
  ldr r1, [r7, #((16+3+5)*4)] @ ICOUNT
  cmp r0, r1
  blo 9f                      @ interrupt pending
  ldr r3, [r7, #144]
  add r3, r3, #1024 @ MAXBLOCKSIZE (ADR-085)
  ldr r0, [r3, #12]           @ last block flags
  tst r0, #1                  @ BLOCK_LOOP: Execute checks for an infinite loop
  bne 9f
  ldr r8, [r7, #((16+3+3)*4)] @ PC
  lsr r0, r8, #20
  cmp r0, #0x060
  bne 9f
  ubfx r0, r8, #1, #19
  ldr r1, [r7, #140]
  ldr r2, [r1, r0, lsl #2]
  cmp r2, #0
  beq 9f
  add r3, r2, #1024 @ MAXBLOCKSIZE (ADR-085)
  ldr r0, [r3, #12]
  tst r0, #4                  @ BLOCK_NOCHAIN
  bne 9f
  str r2, [r7, #144]
  ldr r0, =rstv_act_pc
  str r8, [r0]
  ldr r0, =rstv_dyn_chain
  ldr r1, [r0]
  add r1, r1, #1
  str r1, [r0]
  add r0, r2, #16             @ past the prologue (push, mov r7, ldr PC, ldr COUNT)
  bx r0
9:
  pop {r4-r10, pc}

@ ADR-048: slow path of a linked exit stub (r3 = the stub data: &rstv_link_gen, generation, PC, target block).
@ Same as rstv_chain; when it goes on to the next block it also fills the stub so the next exit jumps directly.
.global rstv_chain_ic
.type rstv_chain_ic, %function
rstv_chain_ic:
  mov ip, r3
  ldr r0, [r7, #136]
  ldr r1, [r0]
  cmp r1, #0
  beq 1f
  add r9, r9, r1
  mov r1, #0
  str r1, [r0]
  str r9, [r7, #((16+3+4)*4)]
1:
  ldr r2, [r7, #128]
  cmp r9, r2
  bhs 9f
  ldr r0, [r7, #(16*4)]
  and r0, r0, #0xF0
  ldr r1, [r7, #((16+3+5)*4)]
  cmp r0, r1
  blo 9f
  ldr r3, [r7, #144]
  add r3, r3, #1024 @ MAXBLOCKSIZE (ADR-085)
  ldr r0, [r3, #12]
  tst r0, #1                  @ the block that ran is BLOCK_LOOP: back to Execute
  bne 9f
  ldr r8, [r7, #((16+3+3)*4)]
  lsr r0, r8, #20
  cmp r0, #0x060
  bne 9f
  ubfx r0, r8, #1, #19
  ldr r1, [r7, #140]
  ldr r2, [r1, r0, lsl #2]
  cmp r2, #0
  beq 9f
  add r3, r2, #1024 @ MAXBLOCKSIZE (ADR-085)
  ldr r0, [r3, #12]
  tst r0, #4                  @ BLOCK_NOCHAIN
  bne 9f
  tst r0, #1                  @ BLOCK_LOOP target: not cached (Execute must see it)
  bne 2f
  ldr r1, [ip]
  ldr r1, [r1]
  str r1, [ip, #4]
  str r8, [ip, #8]
  str r2, [ip, #12]
2:
  str r2, [r7, #144]
  ldr r0, =rstv_act_pc
  str r8, [r0]
  add r0, r2, #16
  bx r0
9:
  pop {r4-r10, pc}

.global rstv_chain_off
.type rstv_chain_off, %function
rstv_chain_off:
  pop {r4-r10, pc}

.ltorg
