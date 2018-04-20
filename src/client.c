/*
 * Copyright (c) 2015 Justin Liu
 * Author: Justin Liu <rssnsj@gmail.com>
 * https://github.com/rssnsj/minivtun
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>

#include "minivtun.h"

static int network_receiving(void)
{
	char read_buffer[NM_PI_BUFFER_SIZE], crypt_buffer[NM_PI_BUFFER_SIZE];
	struct minivtun_msg *nmsg;
	struct tun_pi pi;
	void *out_data;
	size_t ip_dlen, out_dlen;
	struct sockaddr_inx real_peer;
	socklen_t real_peer_alen;
	struct iovec iov[2];
	struct timeval __current;
	int rc;

	gettimeofday(&__current, NULL);

	real_peer_alen = sizeof(real_peer);
	rc = recvfrom(state.sockfd, &read_buffer, NM_PI_BUFFER_SIZE, 0,
			(struct sockaddr *)&real_peer, &real_peer_alen);
	if (rc <= 0)
		return -1;

	out_data = crypt_buffer;
	out_dlen = (size_t)rc;
	netmsg_to_local(read_buffer, &out_data, &out_dlen);
	nmsg = out_data;

	if (out_dlen < MINIVTUN_MSG_BASIC_HLEN)
		return 0;

	/* Verify password. */
	if (memcmp(nmsg->hdr.auth_key, config.crypto_key,
		sizeof(nmsg->hdr.auth_key)) != 0)
		return 0;

	state.last_recv = __current;

	switch (nmsg->hdr.opcode) {
	case MINIVTUN_MSG_IPDATA:
		if (nmsg->ipdata.proto == htons(ETH_P_IP)) {
			/* No packet is shorter than a 20-byte IPv4 header. */
			if (out_dlen < MINIVTUN_MSG_IPDATA_OFFSET + 20)
				return 0;
		} else if (nmsg->ipdata.proto == htons(ETH_P_IPV6)) {
			if (out_dlen < MINIVTUN_MSG_IPDATA_OFFSET + 40)
				return 0;
		} else {
			fprintf(stderr, "*** Invalid protocol: 0x%x.\n", ntohs(nmsg->ipdata.proto));
			return 0;
		}

		ip_dlen = ntohs(nmsg->ipdata.ip_dlen);
		/* Drop incomplete IP packets. */
		if (out_dlen - MINIVTUN_MSG_IPDATA_OFFSET < ip_dlen)
			return 0;

		pi.flags = 0;
		pi.proto = nmsg->ipdata.proto;
		osx_ether_to_af(&pi.proto);
		iov[0].iov_base = &pi;
		iov[0].iov_len = sizeof(pi);
		iov[1].iov_base = (char *)nmsg + MINIVTUN_MSG_IPDATA_OFFSET;
		iov[1].iov_len = ip_dlen;
		rc = writev(state.tunfd, iov, 2);
		break;
	case MINIVTUN_MSG_ECHO_ACK:
		state.last_echo_ack = __current;
		break;
	}

	return 0;
}

static int tunnel_receiving(void)
{
	char read_buffer[NM_PI_BUFFER_SIZE], crypt_buffer[NM_PI_BUFFER_SIZE];
	struct tun_pi *pi = (void *)read_buffer;
	struct minivtun_msg nmsg;
	void *out_data;
	size_t ip_dlen, out_dlen;
	int rc;

	rc = read(state.tunfd, pi, NM_PI_BUFFER_SIZE);
	if (rc < sizeof(struct tun_pi))
		return -1;

	osx_af_to_ether(&pi->proto);

	ip_dlen = (size_t)rc - sizeof(struct tun_pi);

	/* We only accept IPv4 or IPv6 frames. */
	if (pi->proto == htons(ETH_P_IP)) {
		if (ip_dlen < 20)
			return 0;
	} else if (pi->proto == htons(ETH_P_IPV6)) {
		if (ip_dlen < 40)
			return 0;
	} else {
		fprintf(stderr, "*** Invalid protocol: 0x%x.\n", ntohs(pi->proto));
		return 0;
	}

	memset(&nmsg.hdr, 0x0, sizeof(nmsg.hdr));
	nmsg.hdr.opcode = MINIVTUN_MSG_IPDATA;
	nmsg.hdr.seq = htons(state.xmit_seq++);
	memcpy(nmsg.hdr.auth_key, config.crypto_key, sizeof(nmsg.hdr.auth_key));
	nmsg.ipdata.proto = pi->proto;
	nmsg.ipdata.ip_dlen = htons(ip_dlen);
	memcpy(nmsg.ipdata.data, pi + 1, ip_dlen);

	/* Do encryption. */
	out_data = crypt_buffer;
	out_dlen = MINIVTUN_MSG_IPDATA_OFFSET + ip_dlen;
	local_to_netmsg(&nmsg, &out_data, &out_dlen);

	rc = send(state.sockfd, out_data, out_dlen, 0);
	/**
	 * NOTICE: Don't update this on each tunnel packet
	 * transmit. We always need to keep the local virtual IP
	 * (-a local/...) alive.
	 */
	/* last_keepalive = current_ts; */

	return 0;
}

static void send_echo_req(void)
{
	char in_data[64], crypt_buffer[64];
	struct minivtun_msg *nmsg = (struct minivtun_msg *)in_data;
	void *out_msg;
	size_t out_len;

	memset(&nmsg->hdr, 0x0, sizeof(nmsg->hdr));
	nmsg->hdr.opcode = MINIVTUN_MSG_ECHO_REQ;
	nmsg->hdr.seq = htons(state.xmit_seq++);
	memcpy(nmsg->hdr.auth_key, config.crypto_key, sizeof(nmsg->hdr.auth_key));
	nmsg->echo.loc_tun_in = config.local_tun_in;
	nmsg->echo.loc_tun_in6 = config.local_tun_in6;
	nmsg->echo.id = rand();

	out_msg = crypt_buffer;
	out_len = MINIVTUN_MSG_BASIC_HLEN + sizeof(nmsg->echo);
	local_to_netmsg(nmsg, &out_msg, &out_len);

	(void)send(state.sockfd, out_msg, out_len, 0);
}

static int try_resolve_and_connect(const char *peer_addr_pair,
		struct sockaddr_inx *peer_addr)
{
	int sockfd, rc;

	if ((rc = get_sockaddr_inx_pair(peer_addr_pair, peer_addr)) < 0)
		return rc;

	if ((sockfd = socket(peer_addr->sa.sa_family, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		fprintf(stderr, "*** socket() failed: %s.\n", strerror(errno));
		return -1;
	}
	if (connect(sockfd, (struct sockaddr *)peer_addr, sizeof_sockaddr(peer_addr)) < 0) {
		close(sockfd);
		return -EAGAIN;
	}
	set_nonblock(sockfd);

	return sockfd;
}

int run_client(const char *peer_addr_pair)
{
	char s_peer_addr[50];

	if ((state.sockfd = try_resolve_and_connect(peer_addr_pair, &state.peer_addr)) >= 0) {
		/* DNS resolve OK, start service normally. */
		gettimeofday(&state.last_recv, NULL);
		inet_ntop(state.peer_addr.sa.sa_family, addr_of_sockaddr(&state.peer_addr),
				  s_peer_addr, sizeof(s_peer_addr));
		printf("Mini virtual tunneling client to %s:%u, interface: %s.\n",
				s_peer_addr, ntohs(port_of_sockaddr(&state.peer_addr)), config.devname);
	} else if (state.sockfd == -EAGAIN && config.wait_dns) {
		/* Resolve later (state.last_recv = 0). */
		state.last_recv = (struct timeval) { 0, 0 };
		printf("Mini virtual tunneling client, interface: %s. \n", config.devname);
		printf("WARNING: Connection to '%s' temporarily unavailable, "
			   "to be retried later.\n", peer_addr_pair);
	} else if (state.sockfd == -EINVAL) {
		fprintf(stderr, "*** Invalid address pair '%s'.\n", peer_addr_pair);
		return -1;
	} else {
		fprintf(stderr, "*** Unable to connect to '%s'.\n", peer_addr_pair);
		return -1;
	}

	/* Run in background. */
	if (config.in_background)
		do_daemonize();

	if (config.pid_file) {
		FILE *fp;
		if ((fp = fopen(config.pid_file, "w"))) {
			fprintf(fp, "%d\n", (int)getpid());
			fclose(fp);
		}
	}

	/* Trigger the first echo request to be sent. */
	state.last_echo_req = state.last_echo_ack = (struct timeval) { 0, 0 };

	for (;;) {
		fd_set rset;
		struct timeval __current, timeo;
		int rc;

		FD_ZERO(&rset);
		FD_SET(state.tunfd, &rset);
		if (state.sockfd >= 0)
			FD_SET(state.sockfd, &rset);

		timeo = (struct timeval) { 2, 0 };
		rc = select((state.tunfd > state.sockfd ? state.tunfd : state.sockfd) + 1,
					&rset, NULL, NULL, &timeo);
		if (rc < 0) {
			fprintf(stderr, "*** select(): %s.\n", strerror(errno));
			return -1;
		}

		gettimeofday(&__current, NULL);

		/* Fix pertentially corrupted date */
		if (timercmp(&state.last_echo_req, &__current, >))
			state.last_echo_req = __current;
		if (timercmp(&state.last_echo_ack, &__current, >))
			state.last_echo_ack = __current;
		if (timercmp(&state.last_recv, &__current, >))
			state.last_recv = __current;

		/* Send echo request. */
		if (__current.tv_sec - state.last_echo_req.tv_sec > config.keepalive_timeo) {
			if (state.sockfd >= 0) {
				send_echo_req();
				gettimeofday(&state.last_echo_req, NULL);
			}
		}

		/* Connection timed out, try reconnecting. */
		if (__current.tv_sec - state.last_recv.tv_sec > config.reconnect_timeo) {
reconnect:
			/* Reopen the socket for a different local port. */
			if (state.sockfd >= 0)
				close(state.sockfd);
			if ((state.sockfd = try_resolve_and_connect(peer_addr_pair, &state.peer_addr)) < 0) {
				fprintf(stderr, "Unable to connect to '%s', retrying.\n", peer_addr_pair);
				sleep(5);
				goto reconnect;
			}

			gettimeofday(&state.last_recv, NULL);
			/* Trigger the first echo request to be sent */
			state.last_echo_req = state.last_echo_ack = (struct timeval) { 0, 0 };

			inet_ntop(state.peer_addr.sa.sa_family, addr_of_sockaddr(&state.peer_addr),
					s_peer_addr, sizeof(s_peer_addr));
			printf("Reconnected to %s:%u.\n", s_peer_addr,
					ntohs(port_of_sockaddr(&state.peer_addr)));
			continue;
		}

		/* No result from select(), do nothing. */
		if (rc == 0)
			continue;

		if (state.sockfd >= 0 && FD_ISSET(state.sockfd, &rset)) {
			rc = network_receiving();
			if (rc != 0) {
				fprintf(stderr, "Connection went bad. About to reconnect.\n");
				goto reconnect;
			}
		}

		if (FD_ISSET(state.tunfd, &rset)) {
			rc = tunnel_receiving();
			assert(rc == 0);
		}
	}

	return 0;
}
