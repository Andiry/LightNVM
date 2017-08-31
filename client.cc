#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define BUFFER_SIZE 1024

int main(int argc, char **argv)
{
	if (argc <= 2) {
		printf("Usage: %s ip_address remote\n", argv[0]);
		return 1;
	}

	const char *ip = argv[1];
	int remote = atoi(argv[2]);

	struct sockaddr_in server_address;
	bzero(&server_address, sizeof(server_address));
	server_address.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &server_address.sin_addr);
	server_address.sin_port = htons(5555);

	int sock = socket(PF_INET, SOCK_STREAM, 0);
	assert(sock >= 0);

	int ret = connect(sock, (struct sockaddr*)&server_address,
				sizeof(server_address));
	if (ret < 0) {
		printf("errno %d\n", errno);
	} else {
		char buf1[BUFFER_SIZE];
		memset(buf1, 'a', BUFFER_SIZE);
		int send1 = send(sock, buf1, BUFFER_SIZE, 0);
		printf("send %d, %c %c\n", send1, buf1[0], buf1[BUFFER_SIZE - 1]);
		sleep(5);
		char buf2[BUFFER_SIZE];
		int recv1 = recv(sock, buf2, BUFFER_SIZE, 0);
		printf("recv %d, %c %c\n", recv1, buf2[0], buf2[BUFFER_SIZE - 1]);
	}
	close(sock);

	return 0;
}
