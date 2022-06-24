/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * AARCH64 specific definitions for NOLIBC
 * Copyright (C) 2017-2022 Willy Tarreau <w@1wt.eu>
 */

#ifndef _NOLIBC_ARCH_AARCH64_H
#define _NOLIBC_ARCH_AARCH64_H

/* O_* macros for fcntl/open are architecture-specific */
#define O_RDONLY            0
#define O_WRONLY            1
#define O_RDWR              2
#define O_CREAT          0x40
#define O_EXCL           0x80
#define O_NOCTTY        0x100
#define O_TRUNC         0x200
#define O_APPEND        0x400
#define O_NONBLOCK      0x800
#define O_DIRECTORY    0x4000

/* The struct returned by the newfstatat() syscall. Differs slightly from the
 * x86_64's stat one by field ordering, so be careful.
 */
struct sys_stat_struct {
	unsigned long   st_dev;
	unsigned long   st_ino;
	unsigned int    st_mode;
	unsigned int    st_nlink;
	unsigned int    st_uid;
	unsigned int    st_gid;

	unsigned long   st_rdev;
	unsigned long   __pad1;
	long            st_size;
	int             st_blksize;
	int             __pad2;

	long            st_blocks;
	long            st_atime;
	unsigned long   st_atime_nsec;
	long            st_mtime;

	unsigned long   st_mtime_nsec;
	long            st_ctime;
	unsigned long   st_ctime_nsec;
	unsigned int    __unused[2];
};

/* Syscalls for AARCH64 :
 *   - registers are 64-bit
 *   - stack is 16-byte aligned
 *   - syscall number is passed in x8
 *   - arguments are in x0, x1, x2, x3, x4, x5
 *   - the system call is performed by calling svc 0
 *   - syscall return comes in x0.
 *   - the arguments are cast to long and assigned into the target registers
 *     which are then simply passed as registers to the asm code, so that we
 *     don't have to experience issues with register constraints.
 *
 * On aarch64, select() is not implemented so we have to use pselect6().
 */
#define __ARCH_WANT_SYS_PSELECT6

#define my_syscall0(num)                                                      \
({                                                                            \
	register long _num  __asm__ ("x8") = (num);                           \
	register long _arg1 __asm__ ("x0");                                   \
	                                                                      \
	__asm__  volatile (                                                   \
		"svc #0\n"                                                    \
		: "=r"(_arg1)                                                 \
		: "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall1(num, arg1)                                                \
({                                                                            \
	register long _num  __asm__ ("x8") = (num);                           \
	register long _arg1 __asm__ ("x0") = (long)(arg1);                    \
	                                                                      \
	__asm__  volatile (                                                   \
		"svc #0\n"                                                    \
		: "=r"(_arg1)                                                 \
		: "r"(_arg1),                                                 \
		  "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall2(num, arg1, arg2)                                          \
({                                                                            \
	register long _num  __asm__ ("x8") = (num);                           \
	register long _arg1 __asm__ ("x0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("x1") = (long)(arg2);                    \
	                                                                      \
	__asm__  volatile (                                                   \
		"svc #0\n"                                                    \
		: "=r"(_arg1)                                                 \
		: "r"(_arg1), "r"(_arg2),                                     \
		  "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall3(num, arg1, arg2, arg3)                                    \
({                                                                            \
	register long _num  __asm__ ("x8") = (num);                           \
	register long _arg1 __asm__ ("x0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("x1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("x2") = (long)(arg3);                    \
	                                                                      \
	__asm__  volatile (                                                   \
		"svc #0\n"                                                    \
		: "=r"(_arg1)                                                 \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3),                         \
		  "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall4(num, arg1, arg2, arg3, arg4)                              \
({                                                                            \
	register long _num  __asm__ ("x8") = (num);                           \
	register long _arg1 __asm__ ("x0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("x1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("x2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("x3") = (long)(arg4);                    \
	                                                                      \
	__asm__  volatile (                                                   \
		"svc #0\n"                                                    \
		: "=r"(_arg1)                                                 \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4),             \
		  "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall5(num, arg1, arg2, arg3, arg4, arg5)                        \
({                                                                            \
	register long _num  __asm__ ("x8") = (num);                           \
	register long _arg1 __asm__ ("x0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("x1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("x2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("x3") = (long)(arg4);                    \
	register long _arg5 __asm__ ("x4") = (long)(arg5);                    \
	                                                                      \
	__asm__  volatile (                                                   \
		"svc #0\n"                                                    \
		: "=r" (_arg1)                                                \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5), \
		  "r"(_num)                                                   \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define my_syscall6(num, arg1, arg2, arg3, arg4, arg5, arg6)                  \
({                                                                            \
	register long _num  __asm__ ("x8") = (num);                           \
	register long _arg1 __asm__ ("x0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("x1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("x2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("x3") = (long)(arg4);                    \
	register long _arg5 __asm__ ("x4") = (long)(arg5);                    \
	register long _arg6 __asm__ ("x5") = (long)(arg6);                    \
	                                                                      \
	__asm__  volatile (                                                   \
		"svc #0\n"                                                    \
		: "=r" (_arg1)                                                \
		: "r"(_arg1), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5), \
		  "r"(_arg6), "r"(_num)                                       \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

/* startup code */
__asm__ (".section .text\n"
    ".weak _start\n"
    "_start:\n"
    "ldr x0, [sp]\n"              // argc (x0) was in the stack
    "add x1, sp, 8\n"             // argv (x1) = sp
    "lsl x2, x0, 3\n"             // envp (x2) = 8*argc ...
    "add x2, x2, 8\n"             //           + 8 (skip null)
    "add x2, x2, x1\n"            //           + argv
    "and sp, x1, -16\n"           // sp must be 16-byte aligned in the callee
    "bl main\n"                   // main() returns the status code, we'll exit with it.
    "mov x8, 93\n"                // NR_exit == 93
    "svc #0\n"
    "");

#endif // _NOLIBC_ARCH_AARCH64_H
