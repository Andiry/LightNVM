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
#include <unordered_map>
#include <memory>

#include "azure_config.h"
#include "threadpool.h"
#include "ocssd_conn.h"
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

int main(int argc, char **argv)
{
	int ret = 0;
	ret = initialize_ocssd_manager();
	if (ret) {
		printf("OCSSD manager init failed\n");
		return ret;
	}

	addsig(SIGINT, interrupt, false);

	threadpool<ocssd_conn>* pool = NULL;
	try {
		pool = new threadpool<ocssd_conn>;
	} catch (std::exception &e) {
		std::cout << e.what() << std::endl;
		delete manager;
		return -ENOMEM;
	}

	std::unordered_map<int, std::shared_ptr<ocssd_conn>> map;

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

	epoll_event events[MAX_EVENT_NUMBER];
	int epollfd = epoll_create(5);
	addfd(epollfd, listenfd, false);
	ocssd_conn::epollfd = epollfd;
	ocssd_conn::manager = manager;

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
				}

				addfd(epollfd, connfd, true);
				map[connfd] = std::make_shared<ocssd_conn>(connfd, client);
			} else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
				// Close connection
				removefd(epollfd, sockfd);
				map[sockfd] = NULL;
				close(sockfd);
			} else if (events[i].events & EPOLLIN) {
				printf("Sock %d: read request\n", sockfd);
				pool->append(map[sockfd]);
			} else if (events[i].events & EPOLLOUT) {
				printf("Sock %d: write request\n", sockfd);
				pool->append(map[sockfd]);
			}
		}
	}

	printf("Closing...\n");
	close(listenfd);
	manager->persist();
	delete pool;
	delete manager;

	return 0;
}
