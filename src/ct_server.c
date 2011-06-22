/*
 * curvetun - the cipherspace wormhole creator
 * Part of the netsniff-ng project
 * By Daniel Borkmann <daniel@netsniff-ng.org>
 * Copyright 2011 Daniel Borkmann <daniel@netsniff-ng.org>,
 * Subject to the GPL.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <syslog.h>
#include <signal.h>
#include <limits.h>
#include <netdb.h>
#include <sched.h>
#include <ctype.h>
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <arpa/inet.h>

#include "die.h"
#include "netdev.h"
#include "write_or_die.h"
#include "psched.h"
#include "xmalloc.h"
#include "ct_server.h"
#include "curvetun.h"
#include "compiler.h"
#include "trie.h"

struct parent_info {
	int efd;
	int tunfd;
	int ipv4;
	int udp;
};

struct worker_struct {
	int efd;
	unsigned int cpu;
	pthread_t thread;
	struct parent_info parent;
};

static struct worker_struct *threadpool = NULL;

static unsigned int cpus = 0;

extern sig_atomic_t sigint;

static void *worker_tcp(void *self)
{
	int fd;
	uint64_t fd64;
	ssize_t ret, len, err;
	const struct worker_struct *ws = self;
	char buff[1600]; //XXX
	struct pollfd fds;
	size_t nlen;

	fds.fd = ws->efd;
	fds.events = POLLIN;

	syslog(LOG_INFO, "curvetun thread %p/CPU%u up!\n", ws, ws->cpu);
	while (likely(!sigint)) {
		poll(&fds, 1, -1);
		ret = read(ws->efd, &fd64, sizeof(fd64));
		if (ret != sizeof(fd64)) {
			cpu_relax();
			sched_yield();
			continue;
		}
		if (fd64 == ws->parent.tunfd) {
			len = read(ws->parent.tunfd, buff, sizeof(buff));
			if (len > 0) {
				trie_addr_lookup(buff, len, ws->parent.ipv4,
						 &fd, NULL, &nlen);
				if (fd < 0)
					continue;
				err = write(fd, buff, len);
			}
		} else {
			fd = (int) fd64;
			len = read(fd, buff, sizeof(buff));
			if (len > 0) {
				trie_addr_maybe_update(buff, len, ws->parent.ipv4,
						       fd, NULL, 0);
				err = write(ws->parent.tunfd, buff, len);
			} else {
				if (len < 1 && errno != EAGAIN) {
					len = write(ws->parent.efd, &fd64,
						    sizeof(fd64));
					if (len != sizeof(fd64))
						whine("Event write error from thread!\n");
					trie_addr_remove(fd);
				}
			}
		}
	}
	syslog(LOG_INFO, "curvetun thread %p/CPU%u down!\n", ws, ws->cpu);
	pthread_exit(0);
}

static void *worker_udp(void *self)
{
	int fd;
	uint64_t fd64;
	ssize_t ret, len, err;
	const struct worker_struct *ws = self;
	char buff[1600]; //XXX
	struct pollfd fds;
	struct sockaddr_storage naddr;
	socklen_t nlen;

	fds.fd = ws->efd;
	fds.events = POLLIN;

	syslog(LOG_INFO, "curvetun thread %p/CPU%u up!\n", ws, ws->cpu);
	while (likely(!sigint)) {
		poll(&fds, 1, -1);
		ret = read(ws->efd, &fd64, sizeof(fd64));
		if (ret != sizeof(fd64)) {
			cpu_relax();
			sched_yield();
			continue;
		}
		if (fd64 == ws->parent.tunfd) {
			len = read(ws->parent.tunfd, buff, sizeof(buff));
			if (len > 0) {
				nlen = 0;
				memset(&naddr, 0, sizeof(naddr));
				trie_addr_lookup(buff, len, ws->parent.ipv4,
						 &fd, &naddr, (size_t *) &nlen);
				if (fd < 0 || nlen == 0)
					continue;
				err = sendto(fd, buff, len, 0,
					     (struct sockaddr *) &naddr, nlen);
			}
		} else {
			fd = (int) fd64;
			nlen = sizeof(naddr);
			memset(&naddr, 0, sizeof(naddr));
			len = recvfrom(fd, buff, sizeof(buff), 0,
				       (struct sockaddr *) &naddr, &nlen);
			if (len > 0) {
				trie_addr_maybe_update(buff, len, ws->parent.ipv4,
						       fd, &naddr, nlen);
				if (!strncmp(buff, "\r\r\r", strlen("\r\r\r") + 1))
					trie_addr_remove_addr(&naddr, nlen);
				else
					err = write(ws->parent.tunfd, buff, len);
			}
		}
	}
	syslog(LOG_INFO, "curvetun thread %p/CPU%u down!\n", ws, ws->cpu);
	pthread_exit(0);
}

static void tspawn_or_panic(int efd, int tunfd, int ipv4, int udp)
{
	int i, ret;
	cpu_set_t cpuset;
	for (i = 0; i < cpus * THREADS_PER_CPU; ++i) {
		CPU_ZERO(&cpuset);
		threadpool[i].cpu = i % cpus;
		CPU_SET(threadpool[i].cpu, &cpuset);
		threadpool[i].efd = eventfd(0, 0);
		if (threadpool[i].efd < 0)
			panic("Cannot create event socket!\n");
		threadpool[i].parent.efd = efd;
		threadpool[i].parent.tunfd = tunfd;
		threadpool[i].parent.ipv4 = ipv4;
		threadpool[i].parent.udp = udp;

		ret = pthread_create(&(threadpool[i].thread), NULL,
				     udp ? worker_udp : worker_tcp,
				     &threadpool[i]);
		if (ret < 0)
			panic("Thread creation failed!\n");

		ret = pthread_setaffinity_np(threadpool[i].thread,
					     sizeof(cpu_set_t), &cpuset);
		if (ret < 0)
			panic("Thread CPU migration failed!\n");
		pthread_detach(threadpool[i].thread);
	}
}

static void tfinish(void)
{
	int i;
	for (i = 0; i < cpus * THREADS_PER_CPU; ++i) {
		close(threadpool[i].efd);
		pthread_join(threadpool[i].thread, NULL);
	}
}

int server_main(int set_rlim, int port, int lnum)
{
	int lfd = -1, kdpfd, nfds, nfd, ret, curfds, i, trit, efd, tunfd;
	int ipv4 = 0, udp = 1;
	struct epoll_event lev, eev, tev, nev;
	struct epoll_event events[MAX_EPOLL_SIZE];
	struct rlimit rt;
	struct addrinfo hints, *ahead, *ai;
	struct sockaddr_storage taddr;
	socklen_t tlen;

	openlog("curvetun", LOG_PID | LOG_CONS | LOG_NDELAY, LOG_DAEMON);
	syslog(LOG_INFO, "curvetun server booting!\n");

	trie_init();

	cpus = get_number_cpus_online();
	threadpool = xzmalloc(sizeof(*threadpool) * cpus * THREADS_PER_CPU);

	if (set_rlim) {
		rt.rlim_max = rt.rlim_cur = MAX_EPOLL_SIZE;
		ret = setrlimit(RLIMIT_NOFILE, &rt);
		if (ret < 0)
			whine("Cannot set rlimit!\n");
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
	hints.ai_protocol = udp ? IPPROTO_UDP : IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	tunfd = tun_open_or_die(DEVNAME_SERVER);

	ret = getaddrinfo(NULL, "6666", &hints, &ahead);
	if (ret < 0)
		panic("Cannot get address info!\n");

	for (ai = ahead; ai != NULL && lfd < 0; ai = ai->ai_next) {
		lfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (lfd < 0)
			continue;

		if (ai->ai_family == AF_INET6) {
			int one = 1;
#ifdef IPV6_V6ONLY
			ret = setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY,
					 &one, sizeof(one));
			if (ret < 0) {
				close(lfd);
				lfd = -1;
				continue;
			}
#else
			close(lfd);
			lfd = -1;
			continue;
#endif /* IPV6_V6ONLY */
		}

		set_reuseaddr(lfd);

		ret = bind(lfd, ai->ai_addr, ai->ai_addrlen);
		if (ret < 0) {
			close(lfd);
			lfd = -1;
			continue;
		}

		if (!udp) {
			ret = listen(lfd, 5);
			if (ret < 0) {
				close(lfd);
				lfd = -1;
				continue;
			}
		}

		ipv4 = (ai->ai_family == AF_INET6 ? 0 :
			(ai->ai_family == AF_INET ? 1 : -1));
		syslog(LOG_INFO, "curvetun on IPv%d via %s!\n",
		       ipv4 ? 4 : 6, udp ? "UDP" : "TCP");
	}

	freeaddrinfo(ahead);
	if (lfd < 0 || ipv4 < 0)
		panic("Cannot create socket!\n");

	efd = eventfd(0, 0);
	if (efd < 0)
		panic("Cannot create parent event fd!\n");

	set_nonblocking(lfd);
	set_nonblocking(efd);
	set_nonblocking(tunfd);

	kdpfd = epoll_create(MAX_EPOLL_SIZE);
	if (kdpfd < 0)
		panic("Cannot create socket!\n");

	memset(&lev, 0, sizeof(lev));
	lev.events = EPOLLIN | EPOLLET;
	lev.data.fd = lfd;
	memset(&eev, 0, sizeof(lev));
	eev.events = EPOLLIN | EPOLLET;
	eev.data.fd = efd;
	memset(&tev, 0, sizeof(tev));
	tev.events = EPOLLIN | EPOLLET;
	tev.data.fd = tunfd;

	ret = epoll_ctl(kdpfd, EPOLL_CTL_ADD, lfd, &lev);
	if (ret < 0)
		panic("Cannot add socket for epoll!\n");

	ret = epoll_ctl(kdpfd, EPOLL_CTL_ADD, efd, &eev);
	if (ret < 0)
		panic("Cannot add socket for events!\n");

	ret = epoll_ctl(kdpfd, EPOLL_CTL_ADD, tunfd, &tev);
	if (ret < 0)
		panic("Cannot add socket for tundev!\n");

	trit = 0;
	curfds = 3;
	tlen = sizeof(taddr);

	syslog(LOG_INFO, "curvetun up and running!\n");

	tspawn_or_panic(efd, tunfd, ipv4, udp);

	while (likely(!sigint)) {
		nfds = epoll_wait(kdpfd, events, curfds, -1);
		if (nfds < 0) {
			perror("");
			break;
		}

		for (i = 0; i < nfds; ++i) {
			if (events[i].data.fd == lfd && !udp) {
				int one;
				char hbuff[256], sbuff[256];

				nfd = accept(lfd, (struct sockaddr *) &taddr, &tlen);
				if (nfd < 0) {
					perror("accept");
					continue;
				}

				memset(hbuff, 0, sizeof(hbuff));
				memset(sbuff, 0, sizeof(sbuff));

				getnameinfo((struct sockaddr *) &taddr, tlen,
					    hbuff, sizeof(hbuff),
					    sbuff, sizeof(sbuff),
					    NI_NUMERICHOST | NI_NUMERICSERV);

				syslog(LOG_INFO, "New connection from %s:%s with id %d\n",
				       hbuff, sbuff, nfd);

				set_nonblocking(nfd);
				one = 1;
				setsockopt(nfd, SOL_SOCKET, SO_KEEPALIVE,
					   &one, sizeof(one));
				one = 1;
				setsockopt(nfd, IPPROTO_TCP, TCP_NODELAY,
					   &one, sizeof(one));

				memset(&nev, 0, sizeof(nev));
				nev.events = EPOLLIN | EPOLLET;
				nev.data.fd = nfd;

				ret = epoll_ctl(kdpfd, EPOLL_CTL_ADD, nfd, &nev);
				if (ret < 0)
					panic("Epoll ctl error!\n");
				curfds++;
			} else if (events[i].data.fd == efd) {
				uint64_t fd64_del;
				ret = read(efd, &fd64_del, sizeof(fd64_del));
				if (ret != sizeof(fd64_del))
					continue;
				epoll_ctl(kdpfd, EPOLL_CTL_DEL, (int) fd64_del, &nev);
				curfds--;
				syslog(LOG_INFO, "Closed connection with id %d\n",
				       (int) fd64_del);
			} else {
				uint64_t fd64 = events[i].data.fd;
				ret = write(threadpool[trit].efd, &fd64,
					    sizeof(fd64));
				if (ret != sizeof(fd64))
					whine("Write error on event dispatch!\n");
				trit = (trit + 1) % cpus;
			}
		}
	}

	syslog(LOG_INFO, "curvetun prepare shut down!\n");

	close(lfd);
	close(efd);
	close(tunfd);

	tfinish();
	xfree(threadpool);

	trie_cleanup();

	syslog(LOG_INFO, "curvetun shut down!\n");
	closelog();

	return 0;
}

