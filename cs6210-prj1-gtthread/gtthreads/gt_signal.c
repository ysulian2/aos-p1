/*
 * gt_signal.c
 *
 */

#include <stdio.h>
#include <signal.h>
#include <string.h>

#include "gt_signal.h"

/* signal handling */
extern void sig_install_handler_and_unblock(int signo, void (*handler)(int))
{
	/* Setup the handler */
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = handler;
	act.sa_flags = SA_RESTART;
	sigaction(signo, &act,0);

	/* Unblock the signal */
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, signo);
	sigprocmask(SIG_UNBLOCK, &set, NULL);

	return;
}

extern void sig_block_signal(int signo)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, signo);
	sigprocmask(SIG_BLOCK, &set, NULL);
	return;
}

extern void sig_unblock_signal(int signo)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, signo);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
	return;
}
