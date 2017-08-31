/*
 * Copyright (c) 2011, Jason Ish
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
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

/*
 * libevent echo server example.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* For inet_ntoa. */
#include <arpa/inet.h>

/* Required by event.h. */
#include <sys/time.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <err.h>
#include <iostream>
#include <deque>
#include <boost/filesystem.hpp>

/* Libevent. */
#include <event.h>

#include "azure_config.h"
#include "ocssd_server.h"

/* Port to listen on. */
#define SERVER_PORT 5555

/* Length of each buffer in the buffer queue.  Also becomes the amount
 * of data we try to read per call to read(2). */
#define BUFLEN 1024

/**
 * In event based programming we need to queue up data to be written
 * until we are told by libevent that we can write.
 */
struct bufferq {
	/* The buffer. */
	char *buf;

	/* The length of buf. */
	int len;

	/* The offset into buf to start writing from. */
	int offset;
};

/**
 * A struct for client specific data, also includes pointer to create
 * a list of clients.
 *
 * In event based programming it is usually necessary to keep some
 * sort of object per client for state information.
 */
struct client {
	/* Events. We need 2 event structures, one for read event
	 * notification and the other for writing. */
	struct event ev_read;
	struct event ev_write;

	/* This is the queue of data to be written to this client. As
	 * we can't call write(2) until libevent tells us the socket
	 * is ready for writing. */
	std::deque<bufferq *> writeq;
};

struct event_base *base;

void interrupt(int signum) {
	printf("%s\n", __func__);
	event_base_loopbreak(base);
}

static void addsig(int sig, void (*handler)(int), bool restart)
{
	struct sigaction action;
	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	if (restart)
		action.sa_flags |= SA_RESTART;
	sigaction(sig, &action, NULL);
}

ocssd_manager *manager;

static int publish_resource(ocssd_manager *manager)
{
	const std::vector<ocssd_unit *> & ocssds = manager->get_units();

	for (auto unit : ocssds) {
		size_t shared = 0;
		size_t exclusive = 0;
		size_t blocks = 0;
		int ret;

		ret = unit->get_ocssd_stats(shared, exclusive, blocks);

		if (!ret)
			ret = azure_insert_entity(unit->get_desc(),
						shared, exclusive, blocks);
	}

	return ocssds.size();
}

static int initialize_ocssd_manager()
{
	manager = new ocssd_manager();
	if (!manager)
		return -ENOMEM;

	/* Check for existing OCSSDs */
	/* nvme2n1 used as pblk */

	for (int i = 0; i < 2; i++) {
		std::string path = "/dev/nvme" + std::to_string(i) + "n1";

		if (!boost::filesystem::exists(path))
			continue;

		manager->add_ocssd(path);
	}

	publish_resource(manager);
	return 0;
}

/**
 * Set a socket to non-blocking mode.
 */
int
setnonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return flags;
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0)
		return -1;

        return 0;
}

/**
 * This function will be called by libevent when the client socket is
 * ready for reading.
 */
void
on_read(int fd, short ev, void *arg)
{
	struct client *client = (struct client *)arg;
	struct bufferq *bufferq;
	char *buf;
	int len;
	
	/* Because we are event based and need to be told when we can
	 * write, we have to malloc the read buffer and put it on the
	 * clients write queue. */
	buf = (char *)malloc(BUFLEN);
	if (buf == NULL)
		err(1, "malloc failed");

	len = read(fd, buf, BUFLEN);
	if (len == 0) {
		/* Client disconnected, remove the read event and the
		 * free the client structure. */
		printf("Client disconnected.\n");
                close(fd);
		event_del(&client->ev_read);
		free(buf);
		delete client;
		return;
	}
	else if (len < 0) {
		/* Some other error occurred, close the socket, remove
		 * the event and free the client structure. */
		printf("Socket failure, disconnecting client: %s",
		    strerror(errno));
		close(fd);
		event_del(&client->ev_read);
		free(buf);
		delete client;
		return;
	}

	/* We can't just write the buffer back as we need to be told
	 * when we can write by libevent.  Put the buffer on the
	 * client's write queue and schedule a write event. */
	bufferq = new struct bufferq;
	if (bufferq == NULL)
		err(1, "malloc faild");
	bufferq->buf = buf;
	bufferq->len = len;
	bufferq->offset = 0;
	client->writeq.push_back(bufferq);

	/* Since we now have data that needs to be written back to the
	 * client, add a write event. */
	event_add(&client->ev_write, NULL);
}

/**
 * This function will be called by libevent when the client socket is
 * ready for writing.
 */
void
on_write(int fd, short ev, void *arg)
{
	struct client *client = (struct client *)arg;
	struct bufferq *bufferq;
	int len;

	/* Pull the first item off of the write queue. We probably
	 * should never see an empty write queue, but make sure the
	 * item returned is not NULL. */
	bufferq = client->writeq.front();
	if (bufferq == NULL)
		return;

	/* Write the buffer.  A portion of the buffer may have been
	 * written in a previous write, so only write the remaining
	 * bytes. */
        len = bufferq->len - bufferq->offset;
	len = write(fd, bufferq->buf + bufferq->offset,
                    bufferq->len - bufferq->offset);
	if (len == -1) {
		if (errno == EINTR || errno == EAGAIN) {
			/* The write was interrupted by a signal or we
			 * were not able to write any data to it,
			 * reschedule and return. */
			event_add(&client->ev_write, NULL);
			return;
		}
		else {
			/* Some other socket error occurred, exit. */
			err(1, "write");
		}
	}
	else if ((bufferq->offset + len) < bufferq->len) {
		/* Not all the data was written, update the offset and
		 * reschedule the write event. */
		bufferq->offset += len;
		event_add(&client->ev_write, NULL);
		return;
	}

	/* The data was completely written, remove the buffer from the
	 * write queue. */
	client->writeq.pop_front();
	free(bufferq->buf);
	delete bufferq;

	if (client->writeq.size() > 0)
		event_add(&client->ev_write, NULL);
}

/**
 * This function will be called by libevent when there is a connection
 * ready to be accepted.
 */
void
on_accept(int fd, short ev, void *arg)
{
	int client_fd;
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	struct client *client;

	/* Accept the new connection. */
	client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
	if (client_fd == -1) {
		warn("accept failed");
		return;
	}

	/* Set the client socket to non-blocking mode. */
	if (setnonblock(client_fd) < 0)
		warn("failed to set client socket non-blocking");

	/* We've accepted a new client, allocate a client object to
	 * maintain the state of this client. */
	client = new struct client;
	if (client == NULL)
		err(1, "malloc failed");

	/* Setup the read event, libevent will call on_read() whenever
	 * the clients socket becomes read ready.  We also make the
	 * read event persistent so we don't have to re-add after each
	 * read. */
	event_set(&client->ev_read, client_fd, EV_READ|EV_PERSIST, on_read, 
	    client);
	event_base_set(base, &client->ev_read);

	/* Setting up the event does not activate, add the event so it
	 * becomes active. */
	event_add(&client->ev_read, NULL);

	/* Create the write event, but don't add it until we have
	 * something to write. */
	event_set(&client->ev_write, client_fd, EV_WRITE, on_write, client);
	event_base_set(base, &client->ev_write);

	printf("Accepted connection from %s\n",
               inet_ntoa(client_addr.sin_addr));
}

int
main(int argc, char **argv)
{
	int listen_fd;
	struct sockaddr_in listen_addr;
	int reuseaddr_on = 1;
	int ret = 0;

	/* The socket accept event. */
	struct event ev_accept;

	base = event_base_new();
	if (!base)
		return -ENOMEM;

	ret = initialize_ocssd_manager();
	if (ret) {
		printf("OCSSD manager init failed\n");
		event_base_free(base);
		return ret;
	}

	addsig(SIGINT, interrupt, false);

	/* Initialize libevent. */
	event_init();

	/* Create our listening socket. This is largely boiler plate
	 * code that I'll abstract away in the future. */
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
		err(1, "listen failed");
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_on, 
		sizeof(reuseaddr_on)) == -1)
		err(1, "setsockopt failed");
	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin_family = AF_INET;
	listen_addr.sin_addr.s_addr = INADDR_ANY;
	listen_addr.sin_port = htons(OCSSD_MESSAGE_PORT);
	if (bind(listen_fd, (struct sockaddr *)&listen_addr,
		sizeof(listen_addr)) < 0)
		err(1, "bind failed");
	if (listen(listen_fd, 5) < 0)
		err(1, "listen failed");

	std::cout << "Listening on port " << OCSSD_MESSAGE_PORT << "..." << std::endl;

	/* Set the socket to non-blocking, this is essential in event
	 * based programming with libevent. */
	if (setnonblock(listen_fd) < 0)
		err(1, "failed to set server socket to non-blocking");

	/* We now have a listening socket, we create a read event to
	 * be notified when a client connects. */
	event_set(&ev_accept, listen_fd, EV_READ|EV_PERSIST, on_accept, NULL);
	event_base_set(base, &ev_accept);
	event_add(&ev_accept, NULL);

	/* Start the libevent event loop. */
	event_base_dispatch(base);

	event_base_free(base);
	manager->persist();
	delete manager;
	printf("Exit\n");
	return 0;
}
