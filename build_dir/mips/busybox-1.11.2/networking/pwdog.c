/* vi: set sw=4 ts=4: */
/*
 * $Id: ping.c,v 1.56 2004/03/15 08:28:48 andersen Exp $
 * Ping watchdog.
 * Code shamelessly stolen from busybox ping.c
 *
 * Copyright (C) 2006 Kaleda, Ubiquity
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * This version of ping is adapted from the ping in netkit-base 0.10,
 * which is:
 *
 * Copyright (c) 1989 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Mike Muuss.
 *
 * Original copyright notice is retained at the end of this file.
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/signal.h>
#include <sys/syscall.h>
#include <linux/reboot.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include "busybox.h"


static const int MAXIPLEN = 60;
static const int MAXICMPLEN = 76;
#define	MAX_DUP_CHK	(8 * 128)

#define	A(bit)		rcvd_tbl[(bit)>>3]	/* identify byte in array */
#define	B(bit)		(1 << ((bit) & 0x07))	/* identify bit in byte */
#define	CLR(bit)	(A(bit) &= (~B(bit)))

static struct sockaddr_in pingaddr;
static int pingsock = -1;
static const int datalen = 56;

static long ntransmitted, nreceived;


static long timeout = 60;
static long retry_count = 5;
static long	errors = 0;
static char* command = NULL;
static int verbose = 0;
static int initial_sleep = 0;
static int myid;
static int low_mem = 0; //in kB
static char rcvd_tbl[MAX_DUP_CHK / 8];
static struct hostent *hostent;

static void pwd_ping(const char *host);
static void pwd_sendping(int);
static void pwd_unpack(char *, int, struct sockaddr_in *);

/**************************************************************************/
static int in_cksum(unsigned short *buf, int sz)
{
	int nleft = sz;
	int sum = 0;
	unsigned short *w = buf;
	unsigned short ans = 0;

	while (nleft > 1) {
		sum += *w++;
		nleft -= 2;
	}

	if (nleft == 1) {
		*(unsigned char *) (&ans) = *(unsigned char *) w;
		sum += ans;
	}

	sum = (sum >> 16) + (sum & 0xFFFF);
	sum += (sum >> 16);
	ans = ~sum;
	return (ans);
}



static void
pwd_execute_command(void)
{
	syslog(LOG_ALERT, "%ld ping replies missed. Executing `%s`.",
	       errors, command ? command : "reboot");
	if (command)
	    system(command);
	else
	    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		    LINUX_REBOOT_CMD_RESTART, NULL);
}


static void pwd_pingstats(int junk)
{
	int status;

	signal(SIGINT, SIG_IGN);

	syslog(LOG_INFO, "%s stats:  %ld tx,  %ld rx,  %ld err", 
			hostent->h_name, ntransmitted, nreceived, errors);

	if (errors < retry_count)
		status = EXIT_SUCCESS;
	else
		status = EXIT_FAILURE;
	closelog();
        if (command)
	    free (command);
	exit(status);
}

static unsigned int pwd_get_mem(void)
{
    struct sysinfo info;
    if (sysinfo(&info))
	return low_mem;
    return info.freeram / 1024;
}

static void pwd_sendping(int junk)
{
	struct icmp *pkt;
	int i;
	char packet[datalen + 8];

	pkt = (struct icmp *) packet;

	pkt->icmp_type = ICMP_ECHO;
	pkt->icmp_code = 0;
	pkt->icmp_cksum = 0;
	pkt->icmp_seq = ntransmitted++;
	pkt->icmp_id = myid;
	CLR(pkt->icmp_seq % MAX_DUP_CHK);

	gettimeofday((struct timeval *) &packet[8], NULL);
	pkt->icmp_cksum = in_cksum((unsigned short *) pkt, sizeof(packet));

	i = sendto(pingsock, packet, sizeof(packet), 0,
			   (struct sockaddr *) &pingaddr, sizeof(struct sockaddr_in));

	if (i < 0) {
		syslog(LOG_ERR, "error %d in sendto.", errno);
	}

	/* Have we rached errors count limit ? */
	/* I'm incrementing errors count here, and set it to zero, when I receive correct packet */
	if (errors > 0)
		syslog(LOG_WARNING, "Missed %ld ping replies in a row.", errors);
	if ((errors++ >= retry_count) || (pwd_get_mem() < low_mem))
		pwd_execute_command();

	signal(SIGALRM, pwd_sendping);
	/* schedule next in configured timeout */
	alarm(timeout);
}

static void pwd_unpack(char *buf, int sz, struct sockaddr_in *from)
{
	struct icmp *icmppkt;
	struct iphdr *iphdr;
	int hlen;

	/* check IP header */
	iphdr = (struct iphdr *) buf;
	hlen = iphdr->ihl << 2;
	/* discard if too short */
	if (sz < (datalen + ICMP_MINLEN))
		return;

	sz -= hlen;
	icmppkt = (struct icmp *) (buf + hlen);

	if (icmppkt->icmp_id != myid)
	    return;				/* not our ping */

	if (icmppkt->icmp_type == ICMP_ECHOREPLY) {
	    errors = 0;
	    ++nreceived;
	}
}

static void pwd_ping(const char *host)
{
	char packet[datalen + MAXIPLEN + MAXICMPLEN];
	int sockopt;

	pingsock = create_icmp_socket();

	memset(&pingaddr, 0, sizeof(struct sockaddr_in));

	pingaddr.sin_family = AF_INET;
	hostent = xgethostbyname(host);
	if (hostent->h_addrtype != AF_INET)
		bb_error_msg_and_die("unknown address type; only AF_INET is currently supported.");

	memcpy(&pingaddr.sin_addr, hostent->h_addr, sizeof(pingaddr.sin_addr));

	/* enable broadcast pings */
	sockopt = 1;
	setsockopt(pingsock, SOL_SOCKET, SO_BROADCAST, (char *) &sockopt,
			   sizeof(sockopt));

	/* set recv buf for broadcast pings */
	sockopt = 48 * 1024;
	setsockopt(pingsock, SOL_SOCKET, SO_RCVBUF, (char *) &sockopt,
			   sizeof(sockopt));

	syslog(LOG_NOTICE, "PING Watchdog is checking %s (%s).",
	           hostent->h_name,
		   inet_ntoa(*(struct in_addr *) &pingaddr.sin_addr.s_addr));

	signal(SIGINT, pwd_pingstats);

	/* start the ping's going ... */
	pwd_sendping(0);

	/* listen for replies */
	while (1) {
		struct sockaddr_in from;
		socklen_t fromlen = (socklen_t) sizeof(from);
		int c;

		if ((c = recvfrom(pingsock, packet, sizeof(packet), 0,
						  (struct sockaddr *) &from, &fromlen)) < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "error %d in recvfrom", errno);
			continue;
		}
		pwd_unpack(packet, c, &from);
	}
}

extern int pwdog_main(int argc, char **argv)
{
	char *thisarg;
	char* cmd = NULL;
        int do_now = 0;

	errors = 0;
	argc--;
	argv++;
	/* Parse any options */
	while (argc >= 1 && **argv == '-') {
		thisarg = *argv;
		thisarg++;
		switch (*thisarg) {
		case 'e':
			if (--argc <= 0)
			        bb_show_usage();
			argv++;
			cmd = *argv;
			break;
		case 'd':
			if (--argc <= 0)
			        bb_show_usage();
			argv++;
			initial_sleep = atoi(*argv);
			if (initial_sleep <= 0)
				bb_show_usage();
			break;
		case 'm':
			if (--argc <= 0)
			        bb_show_usage();
			argv++;
			low_mem = atoi(*argv);
			if (low_mem < 0)
				bb_show_usage();
			break;
		case 'c':
			if (--argc <= 0)
			        bb_show_usage();
			argv++;
			retry_count = atoi(*argv);
			if (retry_count <= 0)
				bb_show_usage();
			break;
		case 'p':
			if (--argc <= 0)
			        bb_show_usage();
			argv++;
			timeout = atoi(*argv);
			if (timeout <= 0)
				bb_show_usage();
			break;
		case 'v':
			verbose = 1;
			break;
		case 'n':
			do_now = 1;
			break;
		default:
			bb_show_usage();
		}
		argc--;
		argv++;
	}
	if ((argc < 1) && (!do_now))
		bb_show_usage();

	if (cmd != NULL)
		command = strdup(cmd);
	myid = getpid() & 0xFFFF;
	openlog("pwdog", LOG_PID | (verbose ? LOG_PERROR : 0), LOG_DAEMON);
	syslog(LOG_INFO, "pwdog: do_now=%d, initial_sleep=%d, timeout=%ld, retry_count=%ld, low_mem=%u exec=`%s`",
	       do_now, initial_sleep, timeout, retry_count, low_mem, command ? command : "reboot");

	if (!do_now)
	{
	    if (initial_sleep)
		sleep(initial_sleep);


	    pwd_ping(*argv);
	} else {
            pwd_execute_command();
	}

	closelog();
        if (command)
	    free (command);
	return EXIT_SUCCESS;
}



/*
 * Copyright (c) 1989 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Mike Muuss.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. <BSD Advertising Clause omitted per the July 22, 1999 licensing change
 *		ftp://ftp.cs.berkeley.edu/pub/4bsd/README.Impt.License.Change>
 *
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
