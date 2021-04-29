#pragma once

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

/* The CPU state desired by a given call */
typedef struct
{
#ifdef DYNAMIC_REGS
	// The 64-bit regular registers. Allocated all at once
	int64_t *regs;
	// The 128-bit XMM registers. Allocated all at once
	__int128_t *xregs;
#else
	// The 64-bit regular registers
	int64_t regs[6];
	// The 128-bit XMM registers. 
	__int128_t xregs[8];
#endif
	/* Stack arguments. Dynamically allocated since they can be arbitrarily large.
		(uses exponential growing strategy) */
	long *stack;
	// The amount of regular registers used
	size_t regc : 3;
	// The amount of xmm registers used
	size_t xregc : 4; 
	// The amount of stack eightbytes. _technically_ 4 bits too short
	size_t stackc : 57;
}
#ifndef DYNAMIC_REGS
__attribute__((aligned(16)))
#endif
argls;

// Expands the stack of s to fit c additional eightbytes
bool _argls_exp_stack(argls *s, size_t c)
{
	if(((s->stackc + c) & ~s->stackc) > s->stackc)
	{
		// resize
		size_t cap = 1L;

		while(cap <= s->stackc + c)
			cap <<= 1L;

		long *n = realloc(s->stack, cap * 8);
		assert((size_t)n % 16 == 0);

		if(!n)
			return false;

		s->stack = n;
	}

	return true;
}

// Adds a MEMORY class argument
bool _argls_add_memory(argls *s, size_t c, int64_t vals[c])
{
	if(!_argls_exp_stack(s, c))
		return false;

	for (size_t i = 0; i < c; i++)
		s->stack[s->stackc++] = vals[i];

	return true;
}

// Adds an INTEGER class argument
bool _argls_add_integer(argls *s, int64_t v)
{
#ifdef DYNAMIC_REGS
	if(!s->regs && !(s->regs = malloc(8 * 6)))
		return false;
#endif

	if(s->regc == 6)
		return _argls_add_memory(s, 1, &v);
	else
	{
		s->regs[s->regc++] = v;
		return true;
	}
}

// Adds SSE or SSEUP class argument(s), siz bytes in size.
bool _argls_add_sse(argls *s, size_t siz, ...)
{
#ifdef DYNAMIC_REGS
	if(!s->xregs && !(s->xregs = malloc(16 * 8)))
		return false;
#endif
	__int128_t v[8];

	// the stack should be 16-aligned
	assert(((size_t)&v) % 16 == 0);

	asm volatile(
		"movaps %%xmm0, (%0)\n\t"
		"movaps %%xmm1, 16(%0)\n\t"
		"movaps %%xmm2, 32(%0)\n\t"
		"movaps %%xmm3, 48(%0)\n\t"
		"movaps %%xmm4, 64(%0)\n\t"
		"movaps %%xmm5, 80(%0)\n\t"
		"movaps %%xmm6, 96(%0)\n\t"
		"movaps %%xmm7, 112(%0)\n\t"
		:
		: "r" (v)
		: "memory"
	);

	size_t eb = siz/8 + (siz%8 > 0);
	size_t i;

	for (i = 0; i < eb && s->xregc < 8; i += 2)
		s->xregs[s->xregc++] = v[i / 2];
	
	if(i < eb)
		return _argls_add_memory(s, eb - i, (int64_t*)v);
	else
		return true;
}

// Acts like _argls_add_memory but reads c longs from variadic args.
bool _argls_add_memv(argls *s, size_t c, ...)
{
	if(!_argls_exp_stack(s, c))
		return false;

	va_list a;
	va_start(a, c);

	// discard rdx, rcx, r8 and r9
	for (int i = 0; i < 4; i++)
	{
		volatile int discard = va_arg(a, int);
	}
	

	for (size_t i = 0; i < c; i++)
		s->stack[s->stackc++] = va_arg(a, int64_t);
	
	va_end(a);

	return true;
}

#define argls_add_memv(al, x) _argls_add_memv(&al, sizeof(x) / 8 + (sizeof(x) % 8 > 0), x)
#define argls_add_memory(al, x) _argls_add_memory(&al, sizeof(x) / 8 + (sizeof(x) % 8 > 0), (int64_t*)&x)
#define argls_add_integer(al, x) _argls_add_integer(&(al), (int64_t)(x))
#define argls_add_sse(al, x) _argls_add_sse(&al, sizeof(x), x)

#define argls_add(al, x) _Generic((x), \
	void*:		argls_add_integer(al, x), \
	char:		argls_add_integer(al, x), \
	short:		argls_add_integer(al, x), \
	int:		argls_add_integer(al, x), \
	long:		argls_add_integer(al, x), \
	long long:	argls_add_integer(al, x), \
	unsigned char:		argls_add_integer(al, x), \
	unsigned short:		argls_add_integer(al, x), \
	unsigned int:		argls_add_integer(al, x), \
	unsigned long:		argls_add_integer(al, x), \
	unsigned long long:	argls_add_integer(al, x), \
	float:			argls_add_sse(al, x), \
	double:			argls_add_sse(al, x), \
	__float128:		argls_add_sse(al, x), \
	_Decimal32:		argls_add_sse(al, x), \
	_Decimal64:		argls_add_sse(al, x), \
	_Decimal128:	argls_add_sse(al, x), \
	long double:	_argls_add_memv(&al, 2, x) \
)

void call(void *func, argls s)
{
	// prevent type mismatch
	long regc_off = s.regc * 4;
	long xregc_off = s.xregc * 4;

	asm volatile(
		// set up xregc switch case
		//   copy xregs out to rbx because else the register gcc chooses for it would impact the length of movaps
		"mov %[xregs], %%rbx\n\t"
		"movabs $_invoke_switch_xregc, %%rax\n\t"
		"sub %[xregco], %%rax\n\t"
		"jmp *%%rax\n\t"
		// switch case over xregc (8..0)
		// These SHOULD be encoded as 41 0f 28 ?? ?0 but I've had a few problems with that
		"movaps 112(%%rbx), %%xmm7\n\t"
		"movaps 96(%%rbx), %%xmm6\n\t"
		"movaps 80(%%rbx), %%xmm5\n\t"
		"movaps 64(%%rbx), %%xmm4\n\t"
		"movaps 48(%%rbx), %%xmm3\n\t"
		"movaps 32(%%rbx), %%xmm2\n\t"
		"movaps 16(%%rbx), %%xmm1\n\t"
		"movaps 0(%%rbx), %%xmm0\n\t"
		"nop\n\t"
		"_invoke_switch_xregc:\n\t"
		// set up regc switch case
		//   copy regs out to rbx because else the register gcc chooses for it would impact the length of mov
		"mov %[regs], %%rbx\n\t"
		"movabs $_invoke_switch_regc, %%rax\n\t"
		"sub %[regco], %%rax\n\t"
		"jmp *%%rax\n\t"
		// switch case over regc (6..0)
		"mov 40(%%rbx), %%r9\n\t"
		"mov 32(%%rbx), %%r8\n\t"
		"mov 24(%%rbx), %%rcx\n\t"
		"mov 16(%%rbx), %%rdx\n\t"
		"mov 8(%%rbx), %%rsi\n\t"
		"mov (%%rbx), %%rdi\n\t"
		"nop\n\t" // for 4-byte alignment
		"_invoke_switch_regc:\n\t"
		// set up stack
		//  store rsp for gcc weirdness.
		//    rbp is already in use (and is required later to fetch arguments)
		"mov %%rsp, %%rbx\n\t"
		//  ensure 16-alignment will hold after push operation
		"lea (%%rsp, %[stackc], 8), %%rax\n\t"
		"and $15, %%rax\n\t"
		"jz 3f\n\t"
	// god only knows why gcc refuses to assemble push/imm32
	// push 16 bytes of known values onto stack
		"pushw $0xDEAD\n\t"
		"pushw $0xDEAD\n\t"
		"pushw $0xDEAD\n\t"
		"pushw $0xDEAD\n\t"
		"pushw $0xDEAD\n\t"
		"pushw $0xDEAD\n\t"
		"pushw $0xDEAD\n\t"
		"pushw $0xDEAD\n\t"
//		"sub $16, %%rsp\n\t"
		"add %%rax, %%rsp\n\t"
		"3:\n\t"
		//  push arguments
		"1:\n\t"
		"test %[stackc], %[stackc]\n\t"
		"jz 0f\n\t"
		"sub $1, %[stackc]\n\t"
		"push (%[stack], %[stackc], 8)\n\t"
		"jmp 1b\n\t"
		"0:\n\t"
		// dispatch call
		"mov %[xregc], %%al\n\t"
		"call *%[f]\n\t"
		// reset stack pointer
		"mov %%rbx, %%rsp"
		:
		:	[xregs] "g" (s.xregs),	[regco] "g" (regc_off),	[xregc] "g" (s.xregc),
			[regs] "g" (s.regs),	[xregco] "g" (xregc_off),
			[stack] "r" (s.stack), [stackc] "r" (s.stackc),
			// TODO: Investigate the eldritch manifestation when this is "g" as somehow the stored value is corrupted
			[f] "r" (func)
		: "rax", "rbx",
			"rdi", "rsi", "rdx", "rcx", "r8", "r9",
			"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"
	);
}

void argls_end(argls s)
{
#ifdef DYNAMIC_REGS
	free(s.regs);
	free(s.xregs);
#endif

	free(s.stack);
}

void argls_print(argls s)
{
	printf("%hhu general purpose registers\n", s.regc);
	const char *regs[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9", };

	for (int i = 0; i < s.regc; i++)
		printf("	%1$3s: %2$lu/%2$ld/%2$lx\n", regs[i], s.regs[i]);
	
	printf("%hhu SSE vector registers\n", s.xregc);
	
	for (int i = 0; i < s.xregc; i++)
	{
		union
		{
			__int128_t i;
			int64_t qw[2];
			double d[2];
			float s[4];
		} vec;
		vec.i = s.xregs[i];
		printf("	xmm%u: %lx%016lx/[%f,%f]/[%f,%f,%f,%f]\n", 
			i, vec.qw[1], vec.qw[0], vec.d[1], vec.d[0], vec.s[3], vec.s[2], vec.s[1], vec.s[0]);
	}

	printf("%zu stack eightbytes\n", (size_t)s.stackc);

	for (size_t i = 0; i < s.stackc; i++)
	{
		union
		{
			int64_t qw;
			double d;
		} entry;
		entry.qw = s.stack[i];
		printf("	%1$lu/%1$ld/%1$lx/%2$f\n", entry.qw, entry.d);
	}
}

