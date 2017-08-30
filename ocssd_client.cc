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

static int test_local_ocssd(int sock)
{
	char buffer[BUFFER_SIZE];
	memset(buffer, 0, BUFFER_SIZE);

	ocssd_alloc_request request(4, 1024, 1, 0, 0);
	size_t size = request.serialize(buffer);

	int sent = send(sock, buffer, size, 0);
	printf("Request size %lu, sent %d\n", size, sent);

	class virtual_ocssd vssd;
	size = recv(sock, buffer, BUFFER_SIZE, 0);
	printf("Received %lu\n", size);
	vssd.deserialize(buffer);
	const virtual_ocssd_unit *vunit = vssd.get_unit(0);

	for (const virtual_ocssd_channel *vchannel : vunit->get_channels()) {
		printf("channel %u, %u LUNs\n",
			vchannel->get_channel_id(),
			vchannel->get_num_luns());
		if (vchannel->is_shared()) {
			for (const virtual_ocssd_lun * lun : vchannel->get_luns()) {
				printf("LUN %u: block start %u, %u blocks\n",
					lun->get_lun_id(),
					lun->get_block_start(),
					lun->get_num_blocks());
			}
		} else {
			printf("Exclusive channel, %u blocks\n",
				vchannel->get_total_blocks());
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	if (argc <= 1) {
		printf("Usage: %s ip_address\n", argv[0]);
		return 1;
	}

	const char *ip = argv[1];

	struct sockaddr_in server_address;
	bzero(&server_address, sizeof(server_address));
	server_address.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &server_address.sin_addr);
	server_address.sin_port = htons(OCSSD_PORT);

	int sock = socket(PF_INET, SOCK_STREAM, 0);
	assert(sock >= 0);

	int ret = connect(sock, (struct sockaddr*)&server_address,
				sizeof(server_address));
	if (ret < 0) {
		printf("errno %d\n", errno);
	} else {
		test_local_ocssd(sock);
	}

	close(sock);

	return 0;
}
