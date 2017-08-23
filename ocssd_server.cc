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
#include <boost/filesystem.hpp>

#include "azure_config.h"
#include "ocssd_server.h"

#define BUFFER_SIZE 4096

ocssd_manager *manager;

volatile sig_atomic_t stop;

void interrupt(int signum) {
	printf("%s\n", __func__);
	stop = 1;
}

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
	publish_resource(manager);

	delete vssd;
	delete request;
	return 0;
}

static int initialize_ocssd_manager()
{
	manager = new ocssd_manager();
	if (!manager)
		return -ENOMEM;

	/* Check for existing OCSSDs */

	for (int i = 0; i < 9; i++) {
		std::string path = "/dev/nvme" + std::to_string(i) + "n1";

		if (!boost::filesystem::exists(path))
			continue;

		manager->add_ocssd(path);
	}

	publish_resource(manager);
	return 0;
}

int main(int argc, char **argv)
{
	int ret = 0;
	ret = initialize_ocssd_manager();
	if (ret) {
		printf("OCSSD manager init failed\n");
		return ret;
	}

	std::string ip = get_ip();

	struct sockaddr_in address;
	bzero(&address, sizeof(address));
	address.sin_family = AF_INET;
	inet_pton(AF_INET, ip.c_str(), &address.sin_addr);
	address.sin_port = htons(OCSSD_PORT);

	int sock = socket(PF_INET, SOCK_STREAM, 0);
	assert(sock >= 0);

	ret = bind(sock, (struct sockaddr *)&address, sizeof(address));
	assert(ret != -1);

	ret = listen(sock, 5);
	assert(ret != -1);

	std::cout << "Listening on " << ip << ":" << OCSSD_PORT << "..." << std::endl;

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
			char *buffer = new char[BUFFER_SIZE];
			memset(buffer, 0, BUFFER_SIZE);
			int received = 0;
			received = recv(connfd, buffer, BUFFER_SIZE - 1, 0);
			printf("Received %d\n", received);
			process_request(connfd, buffer, received);
			delete[] buffer;
			close(connfd);
		}
	}

	printf("Closing...\n");
	manager->persist();
	delete manager;
	close(sock);

	return 0;
}
