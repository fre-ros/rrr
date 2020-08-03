/*

Read Route Record

Copyright (C) 2018-2020 Atle Solbakken atle@goliathdns.no

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

// Allow SOCK_NONBLOCK on BSD
#define __BSD_VISIBLE 1
#include <sys/socket.h>
#undef __BSD_VISIBLE

#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <netdb.h>
#include <pthread.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "read.h"
#include "log.h"
#include "messages/msg_msg.h"
#include "array.h"
#include "rrr_strerror.h"
#include "read_constants.h"
#include "ip/ip.h"
#include "message_holder/message_holder.h"
#include "message_holder/message_holder_struct.h"
#include "ip/ip_accept_data.h"
#include "ip/ip_util.h"
#include "socket/rrr_socket.h"
#include "socket/rrr_socket_common.h"
#include "messages/msg.h"
#include "socket/rrr_socket_read.h"
#include "socket/rrr_socket_constants.h"
#include "util/rrr_time.h"
#include "util/posix.h"
#include "util/crc32.h"

// TODO : This graylist-stuff is not ip-specific

#define RRR_IP_TCP_GRAYLIST_TIME_MS				2000
#define RRR_IP_TCP_NONBLOCK_CONNECT_TIMEOUT_MS	250

static int __rrr_ip_graylist_exists (
		struct rrr_ip_graylist *list, const struct sockaddr *addr, socklen_t len
) {
	uint64_t time_now = rrr_time_get_64();
	RRR_LL_ITERATE_BEGIN(list, struct rrr_ip_graylist_entry);
		if (time_now > node->expire_time) {
			RRR_LL_ITERATE_SET_DESTROY();
		}
		else if (node->addr_len == len) {
			if (memcmp(&node->addr, addr, len) == 0) {
				return 1;
			}
		}
	RRR_LL_ITERATE_END_CHECK_DESTROY(list, 0; free(node));
	return 0;
}

static int __rrr_ip_graylist_push (
		struct rrr_ip_graylist *target, const struct sockaddr *addr, socklen_t len, int timeout_ms
) {
	int ret = 0;

	struct rrr_ip_graylist_entry *new_entry = NULL;

	if (__rrr_ip_graylist_exists(target, addr, len)) {
		goto out;
	}


	char ip_str[256];
	rrr_ip_to_str(ip_str, 256, addr, len);
	RRR_MSG_0("Host '%s' graylisting for %u ms following connection error\n",
			ip_str,
			RRR_IP_TCP_GRAYLIST_TIME_MS
	);

	if ((new_entry = malloc(sizeof(*new_entry))) == NULL) {
		RRR_MSG_0("Could not allocate memory in __rrr_ip_graylist_push\n");
		ret = 1;
		goto out;
	}

	memset(new_entry, '\0', sizeof(*new_entry));

	if (len > sizeof(new_entry->addr)) {
		RRR_BUG("BUG: address length too long in __rrr_ip_graylist_push %lu > %lu\n",
			len, sizeof(new_entry->addr));
	}

	memcpy (&new_entry->addr, addr, len);
	new_entry->addr_len = len;
	new_entry->expire_time = rrr_time_get_64() + timeout_ms * 1000;

	RRR_LL_APPEND(target, new_entry);
	new_entry = NULL;

	out:
	RRR_FREE_IF_NOT_NULL(new_entry);
	return ret;
}

void rrr_ip_graylist_clear (
		struct rrr_ip_graylist *target
) {
	RRR_LL_DESTROY(target, struct rrr_ip_graylist_entry, free(node));
}

void rrr_ip_graylist_clear_void (
		void *target
) {
	return rrr_ip_graylist_clear(target);
}

struct rrr_ip_receive_callback_data {
	struct rrr_msg_msg_holder *target_entry;
	int (*callback)(struct rrr_msg_msg_holder *entry, void *arg);
	void *callback_arg;
};

static int __rrr_ip_receive_callback (
		struct rrr_read_session *read_session,
		void *arg
) {
	struct rrr_ip_receive_callback_data *callback_data = arg;

	int ret = 0;

	if (read_session->read_complete == 0) {
		RRR_BUG("Read complete was 0 in __ip_receive_packets_callback\n");
	}

	int protocol = 0;

	switch (read_session->socket_options) {
		case SOCK_DGRAM:
			protocol = RRR_IP_UDP;
			break;
		case SOCK_STREAM:
			protocol = RRR_IP_TCP;
			break;
		default:
			RRR_MSG_0("Unknown SO_TYPE %i in __ip_receive_callback\n", read_session->socket_options);
			ret = 1;
			goto out;
	}

	if (callback_data->target_entry->message != NULL) {
		RRR_BUG("message pointer of entry was not empty in __ip_receive_callback\n");
	}

	rrr_msg_msg_holder_set_unlocked (
			callback_data->target_entry,
			read_session->rx_buf_ptr,
			read_session->target_size,
			(const struct sockaddr *) &read_session->src_addr,
			read_session->src_addr_len,
			protocol
	);

	read_session->rx_buf_ptr = NULL;

	ret = callback_data->callback(callback_data->target_entry, callback_data->callback_arg);

	if (ret == 0) {
		// OK
	}
	else if (ret == RRR_IP_RECEIVE_STOP) {
		ret = 0;
		goto out;
	}
	else if (ret == RRR_IP_RECEIVE_ERR) {
		return 1;
	}
	else {
		RRR_BUG("Unknown return value %i from callback in __ip_receive_callback\n", ret);
	}

	out:
	return ret;
}

int rrr_ip_receive_array (
		struct rrr_msg_msg_holder *target_entry,
		struct rrr_read_session_collection *read_session_collection,
		int fd,
		int read_flags,
		const struct rrr_array *definition,
		int do_sync_byte_by_byte,
		unsigned int message_max_size,
		int (*callback)(struct rrr_msg_msg_holder *entry, void *arg),
		void *arg
) {
	struct rrr_ip_receive_callback_data callback_data = {
		target_entry,
		callback,
		arg
	};

	return rrr_socket_common_receive_array (
			read_session_collection,
			fd,
			read_flags,
			RRR_SOCKET_READ_METHOD_RECVFROM,
			definition,
			do_sync_byte_by_byte,
			message_max_size,
			__rrr_ip_receive_callback,
			&callback_data
	);
}

int rrr_ip_send (
		int *err,
		int fd,
		const struct sockaddr *sockaddr,
		socklen_t addrlen,
		void *data,
		ssize_t data_size
) {
	int ret = 0;
	ssize_t bytes = 0;

	*err = 0;

	int max_retries = 100;

	retry:
	if ((bytes = sendto(fd, data, data_size, 0, sockaddr, addrlen)) == -1) {
		*err = errno;
		if (errno == ECONNREFUSED || errno == ECONNRESET) {
			RRR_DBG_1 ("Connection refused in rrr_ip_send\n");
			ret = RRR_SOCKET_SOFT_ERROR;
			goto out;
		}
		else if (errno == EPIPE) {
			RRR_MSG_0 ("Pipe full in ip_send_raw or connection closed by remote\n");
			ret = RRR_SOCKET_SOFT_ERROR;
			goto out;
		}
		else if (--max_retries == 0) {
			RRR_MSG_0 ("Max retries for sendto reached in rrr_ip_send for socket %i pid %i\n",
					fd, getpid());
			ret = RRR_SOCKET_SOFT_ERROR;
			goto out;
		}
		else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			rrr_posix_usleep(10);
			goto retry;
		}
		else if (errno == EINTR) {
			goto retry;
		}

		// Sometimes caller tests for many addresses when looking destination up by hostname
		RRR_DBG_1 ("Note: Error from sendto in rrr_ip_send, address family was %u: %s\n",
				sockaddr->sa_family, rrr_strerror(errno));
		ret = 1;
		goto out;
	}

	// TODO : Possibly handle this situation
	if (bytes != data_size) {
		RRR_MSG_0("All bytes were not sent in sendto in rrr_ip_send\n");
		ret = 1;
		goto out;
	}

	out:
	return ret;
}

void rrr_ip_network_cleanup (
		void *arg
) {
	struct rrr_ip_data *data = arg;
	if (data->fd != 0) {
		rrr_socket_close(data->fd);
		data->fd = 0;
	}
}

int rrr_ip_network_start_udp_ipv4_nobind (
		struct rrr_ip_data *data
) {
	int fd = rrr_socket (
			AF_INET,
			SOCK_DGRAM|SOCK_NONBLOCK,
			IPPROTO_UDP,
			"ip_network_start_udp_ipv4_nobind",
			NULL
	);
	if (fd == -1) {
		RRR_MSG_0 ("Could not create socket: %s\n", rrr_strerror(errno));
		return 1;
	}

	data->fd = fd;

	return 0;
}

int rrr_ip_network_start_udp_ipv4 (
		struct rrr_ip_data *data
) {
	int fd = rrr_socket (
			AF_INET,
			SOCK_DGRAM|SOCK_NONBLOCK,
			IPPROTO_UDP,
			"ip_network_start_udp_ipv4",
			NULL
	);
	if (fd == -1) {
		RRR_MSG_0 ("Could not create socket: %s\n", rrr_strerror(errno));
		goto out_error;
	}

	if (data->port < 1 || data->port > 65535) {
		RRR_MSG_0 ("ip_network_start: port was not in the range 1-65535 (got '%d')\n", data->port);
		goto out_close_socket;
	}

	struct sockaddr_in si;
	memset(&si, '\0', sizeof(si));
	si.sin_family = AF_INET;
	si.sin_port = htons(data->port);
	si.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind (fd, (struct sockaddr *) &si, sizeof(si)) == -1) {
		RRR_MSG_0 ("Could not bind to port %d: %s", data->port, rrr_strerror(errno));
		goto out_close_socket;
	}

	data->fd = fd;

	return 0;

	out_close_socket:
	rrr_socket_close(fd);

	out_error:
	return 1;
}

int rrr_ip_network_sendto_udp_ipv4_or_ipv6 (
		struct rrr_ip_data *ip_data,
		unsigned int port,
		const char *host,
		void *data,
		ssize_t size
) {
	int ret = 0;

	if (port < 1 || port > 65535) {
		RRR_BUG ("rrr_ip_network_udp_sendto: port was not in the range 1-65535 (got '%d')\n", port);
	}

	char port_str[16];
	sprintf(port_str, "%u", port);

	struct addrinfo hints;
	struct addrinfo *result;

	memset (&hints, '\0', sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	int s = getaddrinfo(host, port_str, &hints, &result);
	if (s != 0) {
		RRR_MSG_0("Failed to get address of '%s': %s\n", host, gai_strerror(s));
		ret = 1;
		goto out;
	}

	struct addrinfo *rp;
	int did_send = 0;
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		int err;
		if (rrr_ip_send(&err, ip_data->fd, (struct sockaddr *) rp->ai_addr, rp->ai_addrlen, data, size) == 0) {
			did_send = 1;
			break;
		}
	}

	freeaddrinfo(result);

	if (did_send == 0) {
		RRR_MSG_0("Could not send UDP data to host %s port %u\n", host, port);
		ret = 1;
		goto out;
	}

	out:
	return ret;
}

static int __rrr_ip_network_connect_tcp_check_graylist (
		struct rrr_ip_graylist *graylist,
		const struct sockaddr *addr,
		socklen_t addr_len
) {
	int ret = RRR_SOCKET_OK;

	if (graylist == NULL) {
		goto out;
	}

	if (__rrr_ip_graylist_exists(graylist, addr, addr_len)) {
		ret = RRR_SOCKET_SOFT_ERROR;
		return ret;
	}

	out:
	return ret;
}

int rrr_ip_network_connect_tcp_ipv4_or_ipv6_raw (
		struct rrr_ip_accept_data **accept_data,
		struct sockaddr *addr,
		socklen_t addr_len,
		struct rrr_ip_graylist *graylist
) {
	int fd = 0;

    if (__rrr_ip_network_connect_tcp_check_graylist (graylist, addr, addr_len) != 0) {
    	goto out_error;
    }

	*accept_data = NULL;

	fd = rrr_socket (
			AF_INET,
			SOCK_STREAM|SOCK_NONBLOCK,
			0,
			"ip_network_connect_tcp_ipv4_or_ipv6_raw",
			NULL
	);
	if (fd == -1) {
		RRR_MSG_0("Error while creating socket: %s\n", rrr_strerror(errno));
		goto out_error;
	}

    struct rrr_ip_accept_data *accept_result = malloc(sizeof(*accept_result));
    if (accept_result == NULL) {
    	RRR_MSG_0("Could not allocate memory in ip_network_connect_tcp_ipv4_or_ipv6\n");
    	goto out_close_socket;
    }

    memset(accept_result, '\0', sizeof(*accept_result));

	if (rrr_socket_connect_nonblock(fd, (struct sockaddr *) addr, addr_len) != 0) {
		RRR_DBG_1("Could not connect in in ip_network_connect_tcp_ipv4_or_ipv6\n");
		goto out_free_accept;
	}

	uint64_t timeout = RRR_IP_TCP_NONBLOCK_CONNECT_TIMEOUT_MS * 1000;
	if (rrr_socket_connect_nonblock_postcheck_loop(fd, timeout) != 0) {
		RRR_DBG_1("Connect postcheck failed in ip_network_connect_tcp_ipv4_or_ipv6: %s\n", rrr_strerror(errno));
		goto out_free_accept;
	}

    accept_result->ip_data.fd = fd;
    accept_result->len = addr_len;
    memcpy (&accept_result->addr, addr, addr_len);

    struct sockaddr_in *sockaddr_in = (struct sockaddr_in *) &addr;
    accept_result->ip_data.port = ntohs(sockaddr_in->sin_port);

/*    if (getsockname(fd, &accept_result->addr, &accept_result->len) != 0) {
    	RRR_MSG_0("getsockname failed: %s\n", rrr_strerror(errno));
		goto out_free_accept;
    }*/

    *accept_data = accept_result;

	return 0;

	out_free_accept:
	 	if (graylist != NULL) {
	 		 __rrr_ip_graylist_push(graylist, (struct sockaddr *) addr, addr_len, RRR_IP_TCP_GRAYLIST_TIME_MS);
	 	}
		free(accept_result);
	out_close_socket:
		rrr_socket_close(fd);
	out_error:
		return 1;
}

int rrr_ip_network_connect_tcp_ipv4_or_ipv6 (
		struct rrr_ip_accept_data **accept_data,
		unsigned int port,
		const char *host,
		struct rrr_ip_graylist *graylist
) {
	int fd = 0;

	*accept_data = NULL;

	if (port < 1 || port > 65535) {
		RRR_BUG ("rrr_ip_network_connect_tcp_ipv4_or_ipv6: port was not in the range 1-65535 (got '%d')\n", port);
	}

	char port_str[16];
	sprintf(port_str, "%u", port);

    struct addrinfo hints;
    struct addrinfo *addrinfo_result;

    memset (&hints, '\0', sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int s = getaddrinfo(host, port_str, &hints, &addrinfo_result);
    if (s != 0) {
    	RRR_MSG_0("Failed to get address of '%s': %s\n", host, gai_strerror(s));
    	goto out_error;
    }

    int i = 1;
    struct addrinfo *rp;
    for (rp = addrinfo_result; rp != NULL; rp = rp->ai_next) {
    	fd = rrr_socket (
    			rp->ai_family,
				rp->ai_socktype|SOCK_NONBLOCK,
				rp->ai_protocol,
				"ip_network_connect_tcp_ipv4_or_ipv6",
				NULL
		);
    	if (fd == -1) {
    		RRR_MSG_0("Error while creating socket: %s\n", rrr_strerror(errno));
    		continue;
    	}

    	RRR_DBG_1("Connect attempt with address suggestion #%i to %s:%u address family %u\n",
    			i, host, port, rp->ai_addr->sa_family);

        if (__rrr_ip_network_connect_tcp_check_graylist (graylist, (struct sockaddr *) rp->ai_addr, rp->ai_addrlen) != 0) {
        	goto graylist_next;
        }
        else {
			if (rrr_socket_connect_nonblock(fd, (struct sockaddr *) rp->ai_addr, rp->ai_addrlen) == 0) {
				uint64_t timeout = RRR_IP_TCP_NONBLOCK_CONNECT_TIMEOUT_MS * 1000;
				if (rrr_socket_connect_nonblock_postcheck_loop(fd, timeout) == 0) {
					break;
				}
			}
        }

    	// This means connection refused or some other error, skip to next address suggestion

		if (graylist != NULL) {
			__rrr_ip_graylist_push(graylist, (struct sockaddr *) rp->ai_addr, rp->ai_addrlen, RRR_IP_TCP_GRAYLIST_TIME_MS);
		}

		graylist_next:
    	rrr_socket_close(fd);
    	i++;
    }

    freeaddrinfo(addrinfo_result);

    if (fd < 0 || rp == NULL) {
		RRR_DBG_1 ("Could not connect to host '%s': %s\n", host, (errno != 0 ? rrr_strerror(errno) : "unknown"));
		goto out_error;
    }

    struct rrr_ip_accept_data *accept_result = malloc(sizeof(*accept_result));
    if (accept_result == NULL) {
    	RRR_MSG_0("Could not allocate memory in ip_network_connect_tcp_ipv4_or_ipv6\n");
    	goto out_close_socket;
    }

    memset(accept_result, '\0', sizeof(*accept_result));

    accept_result->ip_data.fd = fd;
    accept_result->ip_data.port = port;
    accept_result->len = sizeof(accept_result->addr);
    if (getsockname(fd, (struct sockaddr *) &accept_result->addr, &accept_result->len) != 0) {
    	RRR_MSG_0("getsockname failed: %s\n", rrr_strerror(errno));
    	goto out_free_accept;
    }

    *accept_data = accept_result;

	return 0;

	out_free_accept:
		free(accept_result);
	out_close_socket:
		rrr_socket_close(fd);
	out_error:
		return 1;
}

int rrr_ip_network_start_tcp_ipv4_and_ipv6 (
		struct rrr_ip_data *data,
		int max_connections
) {
	int fd = rrr_socket (
			AF_INET6,
			SOCK_NONBLOCK|SOCK_STREAM,
			0,
			"ip_network_start",
			NULL
	);
	if (fd == -1) {
		RRR_MSG_0 ("Could not create socket: %s\n", rrr_strerror(errno));
		goto out_error;
	}

	if (data->port < 1 || data->port > 65535) {
		RRR_MSG_0 ("ip_network_start: port was not in the range 1-65535 (got '%d')\n", data->port);
		goto out_close_socket;
	}

	struct sockaddr_in6 si;
	memset(&si, '\0', sizeof(si));
	si.sin6_family = AF_INET6;
	si.sin6_port = htons(data->port);
	si.sin6_addr = in6addr_any;

	if (rrr_socket_bind_and_listen(fd, (struct sockaddr *) &si, sizeof(si), SO_REUSEADDR, max_connections) != 0) {
		RRR_MSG_0 ("Could not listen on port %d: %s\n", data->port, rrr_strerror(errno));
		goto out_close_socket;
	}

	data->fd = fd;

	return 0;

	out_close_socket:
	rrr_socket_close(fd);

	out_error:
	return 1;
}

int rrr_ip_close (
		struct rrr_ip_data *data
) {
	if (data->fd == 0) {
		RRR_BUG("Received zero-value FD in ip_close\n");
	}
	int ret = rrr_socket_close(data->fd);

	data->fd = 0;

	return ret;
}

int rrr_ip_accept (
		struct rrr_ip_accept_data **accept_data,
		struct rrr_ip_data *listen_data,
		const char *creator,
		int tcp_nodelay
) {
	int ret = 0;

	struct sockaddr_storage sockaddr_tmp = {0};
	socklen_t socklen_tmp = sizeof(sockaddr_tmp);
	struct rrr_ip_accept_data *res = NULL;

	*accept_data = NULL;

	ret = rrr_socket_accept(listen_data->fd, (struct sockaddr *) &sockaddr_tmp, &socklen_tmp, creator);
	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			ret = 0;
			goto out;
		}
		else {
			RRR_MSG_0("Error in ip_accept: %s\n", rrr_strerror(errno));
			ret = 1;
			goto out;
		}
	}

//	char buf[256];
//	rrr_ip_to_str(buf, sizeof(buf), (struct sockaddr *) &sockaddr_tmp, socklen_tmp);
//	printf("ip accept: %s family %i\n", buf, sockaddr_tmp.ss_family);

	int fd = ret;
	ret = 0;

	int enable = 1;
	if (tcp_nodelay == 1) {
		if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) != 0) {
			RRR_MSG_0("Could not set TCP_NODELAY for socket in ip_accept: %s\n", rrr_strerror(errno));
			ret = 1;
			goto out_close_socket;
		}
	}

	if (setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0) {
		RRR_MSG_0 ("Could not set SO_REUSEADDR for accepted connection: %s\n", rrr_strerror(errno));
		goto out_close_socket;
	}

	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) {
		RRR_MSG_0("Error while getting flags with fcntl for socket: %s\n", rrr_strerror(errno));
		goto out_close_socket;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		RRR_MSG_0("Error while setting O_NONBLOCK on socket: %s\n", rrr_strerror(errno));
		goto out_close_socket;
	}

	res = malloc(sizeof(*res));
	if (res == NULL) {
		RRR_MSG_0("Could not allocate memory in ip_accept\n");
		ret = 1;
		goto out;
	}
	memset (res, '\0', sizeof(*res));

	if (	((struct sockaddr *) &sockaddr_tmp)->sa_family != AF_INET &&
			((struct sockaddr *) &sockaddr_tmp)->sa_family != AF_INET6
	) {
		RRR_BUG("Non AF_INET/AF_INET6 from accept() in ip_accept\n");
	}

	res->ip_data.fd = fd;
	res->addr = sockaddr_tmp;
	res->len = socklen_tmp;

	{
		struct sockaddr_in client_tmp;
		socklen_t len_tmp = sizeof(client_tmp);
		getpeername(res->ip_data.fd, (struct sockaddr *) &client_tmp, &len_tmp);
	    res->ip_data.port = ntohs (client_tmp.sin_port);
	}

	*accept_data = res;
	res = NULL;

	goto out;

	out_close_socket:
		rrr_socket_close(fd);

	out:
		RRR_FREE_IF_NOT_NULL(res);
		return ret;
}