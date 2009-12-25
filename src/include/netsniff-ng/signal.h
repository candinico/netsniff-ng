/* 
 * netsniff-ng
 *
 * High performance network sniffer for packet inspection
 *
 * Copyright (C) 2009, 2010  Daniel Borkmann <danborkmann@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation; either version 2 of the License, or (at 
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY 
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 51 Franklin St, Fifth Floor, Boston, MA 02110, USA
 *
 * Note: Your kernel has to be compiled with CONFIG_PACKET_MMAP=y option in 
 *       order to use this.
 */

/*
 * Contains: 
 *    Signal related stuff
 */

#ifndef _NET_SIGNAL_H_
#define _NET_SIGNAL_H_

#include <stdarg.h>
#include <signal.h>

/* Function signatures */
/* XXX: All stuff is inlined ... since it is not _always_inline we let gcc 
        decide whether to inline or not */

static void register_softirq(int sig, void (*softirq_handler)(int));
static inline void hold_softirq(int num_count, ...);
static inline void restore_softirq(int num_count, ...);
static inline void hold_softirq_pthread(int num_count, ...);

/**
 * register_softirq - Registers signal + signal handler function
 * @signal:          signal number
 * @softirq_handler: signal handler function
 */
static inline void register_softirq(int signal, void (*softirq_handler)(int))
{
        sigset_t block_mask;
        struct sigaction saction;

        sigfillset(&block_mask);

        saction.sa_handler = softirq_handler;
        saction.sa_mask = block_mask;
        saction.sa_flags = SA_RESTART;
        
        sigaction(signal, &saction, NULL);
}

/**
 * hold_softirq - Set defined signals to blocking 
 * @...:         signals (type of int)
 */
static inline void hold_softirq(int num_count, ...)
{
        int i;
        int signal;

        va_list al;
        sigset_t block_mask;

        sigemptyset(&block_mask);
        va_start(al, num_count);

        for(i = 1; i <= num_count; ++i)
        {
                signal = va_arg(al, int);
                sigaddset(&block_mask, signal);
        }

        sigprocmask(SIG_BLOCK, &block_mask, NULL);
}

/**
 * restore_softirq - Unblocks and delivers pending signals
 * @...:            signals (type of int)
 */
static inline void restore_softirq(int num_count, ...)
{
        int i;
        int signal;

        va_list al;
        sigset_t block_mask;

        sigemptyset(&block_mask);
        va_start(al, num_count);

        for(i = 1; i <= num_count; ++i)
        {
                signal = va_arg(al, int);
                sigaddset(&block_mask, signal);
        }

        sigprocmask(SIG_UNBLOCK, &block_mask, NULL);
}

/**
 * hold_softirq - Set defined signals to blocking (for POSIX threads)
 * @...:         signals (type of int)
 */
static inline void hold_softirq_pthread(int num_count, ...)
{
        int i;
        int signal;

        va_list al;
        sigset_t block_mask;

        sigemptyset(&block_mask);
        va_start(al, num_count);

        for(i = 1; i <= num_count; ++i)
        {
                signal = va_arg(al, int);
                sigaddset(&block_mask, signal);
        }

        pthread_sigmask(SIG_BLOCK, &block_mask, NULL);
}

#endif /* _NET_SIGNAL_H_ */