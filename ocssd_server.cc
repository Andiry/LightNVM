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
#include <sys/epoll.h>
#include <boost/filesystem.hpp>

#include "azure_config.h"
#include "ocssd_server.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define BUFFER_SIZE 4096

ocssd_manager *manager;

volatile sig_atomic_t stop;

void interrupt(int signum) {
	printf("%s\n", __func__);
	stop = 1;
}

static int setnonblocking(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

static void addfd(int epollfd, int fd, bool one_shot)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	if (one_shot)
		event.events |= EPOLLONESHOT;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
	setnonblocking(fd);
}

static void removefd(int epollfd, int fd)
{
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}

static void modfd(int epollfd, int fd, int ev)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = ev | EPOLLONESHOT | EPOLLET | EPOLLRDHUP;
	epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
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

	int listenfd = socket(PF_INET, SOCK_STREAM, 0);
	assert(listenfd >= 0);
	struct linger tmp = {1, 0};
	setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

	ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
	assert(ret != -1);

	ret = listen(listenfd, 5);
	assert(ret != -1);

	std::cout << "Listening on " << ip << ":" << OCSSD_PORT << "..." << std::endl;

	struct sigaction action;
	action.sa_handler = interrupt;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL);

	epoll_event events[MAX_EVENT_NUMBER];
	int epollfd = epoll_create(5);
	addfd(epollfd, listenfd, false);

	while (!stop) {
		int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
		if (number < 0 && errno != EINTR) {
			printf("epoll failure\n");
			break;
		}

		for (int i = 0; i < number; i++) {
			int sockfd = events[i].data.fd;
			if (sockfd == listenfd) {
				struct sockaddr_in client;
				socklen_t client_addrlen = sizeof(client);

				int connfd = accept(listenfd, (struct sockaddr*)&client,
								&client_addrlen);

				if (connfd < 0) {
					printf("errno %d\n", errno);
					continue;
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
			} else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
				// Close conn
			} else if (events[i].events & EPOLLIN) {
				printf("Sock %d: read request\n", sockfd);
			} else if (events[i].events & EPOLLOUT) {
				printf("Sock %d: write request\n", sockfd);
			}
		}
	}

	printf("Closing...\n");
	manager->persist();
	delete manager;
	close(listenfd);

	return 0;
}
