#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "ocssd_server.h"

#define BUFFER_SIZE 1024

static int process_request(const char *buffer, int size)
{
	class ocssd_alloc_request *request;

	if (size != sizeof(class ocssd_alloc_request)) {
		printf("Received size incorrect: %d\n", size);
		return -1;
	}

	request = (class ocssd_alloc_request *)buffer;

	if (request->magic_ != OCSSD_MAGIC) {
		printf("MAGIC does not match\n");
		return -1;
	}

	printf("Request %d channels, shared %d, NUMA ID %d\n",
		request->num_channels_,
		request->shared_,
		request->numa_id_);

	return 0;
}

int main(int argc, char **argv)
{
	if (argc <= 2) {
		printf("Usage: %s ip_address port_number\n", argv[0]);
		return 1;
	}

	const char *ip = argv[1];
	int port = atoi(argv[2]);

	struct sockaddr_in address;
	bzero(&address, sizeof(address));
	address.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &address.sin_addr);
	address.sin_port = htons(port);

	int sock = socket(PF_INET, SOCK_STREAM, 0);
	assert(sock >= 0);

	int ret = bind(sock, (struct sockaddr *)&address, sizeof(address));
	assert(ret != -1);

	ret = listen(sock, 5);
	assert(ret != -1);

	struct sockaddr_in client;
	socklen_t client_addrlen = sizeof(client);

	while (1) {
		int connfd = accept(sock, (struct sockaddr*)&client, &client_addrlen);
		if (connfd < 0) {
			printf("errno %d\n", errno);
		} else {
			char buffer[BUFFER_SIZE];
			memset(buffer, '\0', BUFFER_SIZE);
			int received = 0;
			received = recv(connfd, buffer, BUFFER_SIZE - 1, 0);
			printf("Received %d\n", received);
			process_request(buffer, received);

			virtual_ocssd vssd;
			vssd.count++;

			int sent = send(connfd, &vssd, sizeof(virtual_ocssd), 0);
			printf("Sent %d, count %d\n", sent, vssd.count);

			close(connfd);
		}
	}

	close(sock);

	return 0;
}
