#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

#include "ocssd_server.h"

#define BUFFER_SIZE 1024

ocssd_manager *manager;

volatile sig_atomic_t stop;

void interrupt(int signum) {
	printf("%s\n", __func__);
	stop = 1;
}

static int process_request(int connfd, char *buffer, int size)
{
	ocssd_alloc_request *request = new ocssd_alloc_request(buffer);
	virtual_ocssd *vssd = new virtual_ocssd();
	size_t ret = 0;

	ret = manager->alloc_ocssd_resource(vssd, request);

	size_t len = vssd->serialize(buffer);

	int sent = send(connfd, buffer, len, 0);
	printf("Alloc %lu channels, len %lu, sent %d\n", ret, len, sent);

	vssd->print();
	manager->persist();

	delete vssd;
	delete request;
	return 0;
}

static int initialize_ocssd_manager()
{
	manager = new ocssd_manager();
	if (!manager)
		return -ENOMEM;

	manager->add_ocssd("/dev/nvme0n1");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc <= 2) {
		printf("Usage: %s ip_address port_number\n", argv[0]);
		return 1;
	}

	int ret = 0;
	ret = initialize_ocssd_manager();
	if (ret) {
		printf("OCSSD manager init failed\n");
		return ret;
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

	ret = bind(sock, (struct sockaddr *)&address, sizeof(address));
	assert(ret != -1);

	ret = listen(sock, 5);
	assert(ret != -1);

	printf("Listening...\n");

	struct sockaddr_in client;
	socklen_t client_addrlen = sizeof(client);

	struct sigaction action;
	action.sa_handler = interrupt;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL);

	while (!stop) {
		int connfd = accept(sock, (struct sockaddr*)&client, &client_addrlen);
		if (connfd < 0) {
			printf("errno %d\n", errno);
		} else {
			char buffer[BUFFER_SIZE];
			memset(buffer, 0, BUFFER_SIZE);
			int received = 0;
			received = recv(connfd, buffer, BUFFER_SIZE - 1, 0);
			printf("Received %d\n", received);
			process_request(connfd, buffer, received);
			close(connfd);
		}
	}

	printf("Closing...\n");
	manager->persist();
	delete manager;
	close(sock);

	return 0;
}
