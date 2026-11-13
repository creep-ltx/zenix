/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): ZEX2: more than 32 entries reached with 8-bit offsets (must be refused) */
	.section .text.main,"ax"
	.globl main
main:
	move.l (v1@GOT.b,%a5,%d0.l),%a0
	move.l (v2@GOT.b,%a5,%d0.l),%a0
	move.l (v3@GOT.b,%a5,%d0.l),%a0
	move.l (v4@GOT.b,%a5,%d0.l),%a0
	move.l (v5@GOT.b,%a5,%d0.l),%a0
	move.l (v6@GOT.b,%a5,%d0.l),%a0
	move.l (v7@GOT.b,%a5,%d0.l),%a0
	move.l (v8@GOT.b,%a5,%d0.l),%a0
	move.l (v9@GOT.b,%a5,%d0.l),%a0
	move.l (v10@GOT.b,%a5,%d0.l),%a0
	move.l (v11@GOT.b,%a5,%d0.l),%a0
	move.l (v12@GOT.b,%a5,%d0.l),%a0
	move.l (v13@GOT.b,%a5,%d0.l),%a0
	move.l (v14@GOT.b,%a5,%d0.l),%a0
	move.l (v15@GOT.b,%a5,%d0.l),%a0
	move.l (v16@GOT.b,%a5,%d0.l),%a0
	move.l (v17@GOT.b,%a5,%d0.l),%a0
	move.l (v18@GOT.b,%a5,%d0.l),%a0
	move.l (v19@GOT.b,%a5,%d0.l),%a0
	move.l (v20@GOT.b,%a5,%d0.l),%a0
	move.l (v21@GOT.b,%a5,%d0.l),%a0
	move.l (v22@GOT.b,%a5,%d0.l),%a0
	move.l (v23@GOT.b,%a5,%d0.l),%a0
	move.l (v24@GOT.b,%a5,%d0.l),%a0
	move.l (v25@GOT.b,%a5,%d0.l),%a0
	move.l (v26@GOT.b,%a5,%d0.l),%a0
	move.l (v27@GOT.b,%a5,%d0.l),%a0
	move.l (v28@GOT.b,%a5,%d0.l),%a0
	move.l (v29@GOT.b,%a5,%d0.l),%a0
	move.l (v30@GOT.b,%a5,%d0.l),%a0
	move.l (v31@GOT.b,%a5,%d0.l),%a0
	move.l (v32@GOT.b,%a5,%d0.l),%a0
	move.l (v33@GOT.b,%a5,%d0.l),%a0
	move.l (v34@GOT.b,%a5,%d0.l),%a0
	move.l (v35@GOT.b,%a5,%d0.l),%a0
	move.l (v36@GOT.b,%a5,%d0.l),%a0
	move.l (v37@GOT.b,%a5,%d0.l),%a0
	move.l (v38@GOT.b,%a5,%d0.l),%a0
	move.l (v39@GOT.b,%a5,%d0.l),%a0
	move.l (v40@GOT.b,%a5,%d0.l),%a0
	rts
	.section .data.v,"aw"
v1:	.long 1
v2:	.long 2
v3:	.long 3
v4:	.long 4
v5:	.long 5
v6:	.long 6
v7:	.long 7
v8:	.long 8
v9:	.long 9
v10:	.long 10
v11:	.long 11
v12:	.long 12
v13:	.long 13
v14:	.long 14
v15:	.long 15
v16:	.long 16
v17:	.long 17
v18:	.long 18
v19:	.long 19
v20:	.long 20
v21:	.long 21
v22:	.long 22
v23:	.long 23
v24:	.long 24
v25:	.long 25
v26:	.long 26
v27:	.long 27
v28:	.long 28
v29:	.long 29
v30:	.long 30
v31:	.long 31
v32:	.long 32
v33:	.long 33
v34:	.long 34
v35:	.long 35
v36:	.long 36
v37:	.long 37
v38:	.long 38
v39:	.long 39
v40:	.long 40
