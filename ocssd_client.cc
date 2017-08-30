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

static int test_erase_block(int sock, uint32_t block_idx)
{
	ocssd_io_request request(ERASE_BLOCK_REQUEST, block_idx, 0, 0);
	char buffer[BUFFER_SIZE];

	size_t size = request.serialize(buffer);

	int sent = send(sock, buffer, size, 0);
	printf("%s: Request size %lu, sent %d\n", __func__, size, sent);

	return 0;
}

static int test_write_block(int sock, uint32_t block_idx, size_t count)
{
	ocssd_io_request request(WRITE_BLOCK_REQUEST, block_idx, count, 0);
	char buffer[BUFFER_SIZE];

	size_t size = request.serialize(buffer);

	int sent = send(sock, buffer, size, 0);
	printf("%s: Request size %lu, sent %d\n", __func__, size, sent);

	char* data_buf = (char *)malloc(count);

	memset(data_buf, 'a', count);
	sent = send(sock, data_buf, count, 0);
	printf("%s: write size %lu, sent %d\n", __func__, size, sent);

	free(data_buf);
	return 0;
}

static int test_read_block(int sock, uint32_t block_idx, size_t count, size_t offset)
{
	ocssd_io_request request(READ_BLOCK_REQUEST, block_idx, count, offset);
	char buffer[BUFFER_SIZE];

	size_t size = request.serialize(buffer);

	int sent = send(sock, buffer, size, 0);
	printf("%s: Request size %lu, sent %d\n", __func__, size, sent);

	char* data_buf = (char *)malloc(count);

	memset(data_buf, 'b', count);
	sent = recv(sock, data_buf, count, 0);
	printf("%s: read size %lu, recv %d, %c %c\n", __func__, size, sent,
			data_buf[0], data_buf[count - 1]);

	free(data_buf);
	return 0;
}

static int test_remote_ocssd(int sock)
{
	char buffer[BUFFER_SIZE];
	memset(buffer, 0, BUFFER_SIZE);

	ocssd_alloc_request request(4, 1024, 1, 0, 1);
	size_t size = request.serialize(buffer);

	int sent = send(sock, buffer, size, 0);
	printf("Request size %lu, sent %d\n", size, sent);

	class virtual_ocssd vssd;
	size = recv(sock, buffer, BUFFER_SIZE, 0);
	printf("Received %lu\n", size);
	vssd.deserialize(buffer);

	test_erase_block(sock, 0);
	test_write_block(sock, 0, 32768);
	test_read_block(sock, 0, 32768, 0);

	return 0;
}

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
	server_address.sin_port = htons(OCSSD_MESSAGE_PORT);

	int sock = socket(PF_INET, SOCK_STREAM, 0);
	assert(sock >= 0);

	int ret = connect(sock, (struct sockaddr*)&server_address,
				sizeof(server_address));
	if (ret < 0) {
		printf("errno %d\n", errno);
	} else if (remote == 0) {
		test_local_ocssd(sock);
	} else {
		test_remote_ocssd(sock);
	}

	close(sock);

	return 0;
}
