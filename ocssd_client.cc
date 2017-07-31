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

int main(int argc, char **argv)
{
	if (argc <= 2) {
		printf("Usage: %s ip_address port_number\n", argv[0]);
		return 1;
	}

	const char *ip = argv[1];
	int port = atoi(argv[2]);

	struct sockaddr_in server_address;
	bzero(&server_address, sizeof(server_address));
	server_address.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &server_address.sin_addr);
	server_address.sin_port = htons(port);

	int sock = socket(PF_INET, SOCK_STREAM, 0);
	assert(sock >= 0);

	int ret = connect(sock, (struct sockaddr*)&server_address,
				sizeof(server_address));
	if (ret < 0) {
		printf("errno %d\n", errno);
	} else {
		char buffer[BUFFER_SIZE];
		memset(buffer, 0, BUFFER_SIZE);

		ocssd_alloc_request request(4);
		size_t size = request.serialize(buffer);

		int sent = send(sock, buffer, size, 0);
		printf("Request size %lu, sent %d\n", size, sent);

		class virtual_ocssd vssd;
		size = recv(sock, buffer, BUFFER_SIZE, 0);
		printf("Received %lu\n", size);
		vssd.deserialize(buffer);
	}

	close(sock);

	return 0;
}
