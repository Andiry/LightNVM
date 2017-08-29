#ifndef OCSSD_CONN_H
#define OCSSD_CONN_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <exception>

#include "azure_config.h"
#include "ocssd_server.h"

#define READ_BUFFER_SIZE 4096

enum REQUEST_CODE {
	NO_REQUEST = 0,
	ALLOC_VSSD_REQUEST,
};

class ocssd_conn {
public:
	ocssd_conn(int connfd, const struct sockaddr_in &client);
	~ocssd_conn();

	void close_conn(bool real_close = true);
	void process();
	bool read();
	bool write();

	static int epollfd;
	static ocssd_manager *manager;

private:
	ocssd_conn(const ocssd_conn &);
	ocssd_conn & operator=(const ocssd_conn &);

	REQUEST_CODE process_read();
	bool process_write(REQUEST_CODE code);
	REQUEST_CODE parse_buffer(char *temp_buf, int size);
	int process_alloc_request(char *buffer);
	int publish_resource();

	std::mutex mutex_;
	int connfd_;
	std::string ipaddr_;
	int read_start_;
	int read_end_;
	char read_buf_[READ_BUFFER_SIZE];

	/* Keep a virtual ssd here for remote access */
	int remote_vssd_;
	virtual_ocssd *vssd_;
};

ocssd_conn::ocssd_conn(int connfd, const struct sockaddr_in &client)
	: connfd_(connfd), ipaddr_(inet_ntoa(client.sin_addr)),
	read_start_(0), read_end_(0), remote_vssd_(0), vssd_(NULL)
{
	std::cout << "New connection: conn " << connfd_
		<< ", IP addr " << ipaddr_ << std::endl;
}

ocssd_conn::~ocssd_conn()
{
	printf("%s\n", __func__);
	if (remote_vssd_)
		delete vssd_;
}

void ocssd_conn::close_conn(bool real_close)
{
	if (real_close && connfd_ != -1) {
		removefd(epollfd, connfd_);
		connfd_ = -1;
	}
}

void ocssd_conn::process()
{
	REQUEST_CODE read_ret = NO_REQUEST;

	if (read()) {
		read_ret = process_read();
		if (read_ret == NO_REQUEST) {
			modfd(epollfd, connfd_, EPOLLIN);
			return;
		}
	}

	bool write_ret = process_write(read_ret);
	if (!write_ret)
		close_conn();

	modfd(epollfd, connfd_, EPOLLOUT);
}

bool ocssd_conn::read()
{
	MutexLock lock(&mutex_);
	if (read_end_ >= READ_BUFFER_SIZE)
		return false;

	int bytes_read = 0;
	while (true) {
		bytes_read = recv(connfd_, read_buf_ + read_end_,
					READ_BUFFER_SIZE - read_end_, 0);
		if (bytes_read == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return false;
		} else if (bytes_read == 0) {
			return false;
		}

		read_end_ += bytes_read;
	}

	return true;
}

bool ocssd_conn::write()
{
	return true;
}

REQUEST_CODE ocssd_conn::parse_buffer(char *temp_buf, int size)
{
	uint32_t header = get_header(temp_buf);
	REQUEST_CODE ret = NO_REQUEST;

	switch (header) {
	case REQUEST_MAGIC:
		if (size < REQUEST_ALLOC_SIZE)
			break;
		ret = ALLOC_VSSD_REQUEST;
		read_start_ += REQUEST_ALLOC_SIZE;
		if (read_start_ == read_end_)
			read_start_ = read_end_ = 0;
		break;
	default:
		break;
	}

	return ret;
}

REQUEST_CODE ocssd_conn::process_read()
{
	char temp_buf[1024];
	REQUEST_CODE code = NO_REQUEST;

	{
		MutexLock lock(&mutex_);
		printf("Received %d bytes\n", read_end_ - read_start_);

		memcpy(temp_buf, read_buf_ + read_start_, read_end_ - read_start_);
		code = parse_buffer(temp_buf, read_end_ - read_start_);
	}

	switch (code) {
	case ALLOC_VSSD_REQUEST:
		process_alloc_request(temp_buf);
		break;
	default:
		break;
	}

	return NO_REQUEST;
}

int ocssd_conn::publish_resource()
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

int ocssd_conn::process_alloc_request(char *buffer)
{
	ocssd_alloc_request *request = new ocssd_alloc_request(buffer);
	virtual_ocssd *vssd = new virtual_ocssd();
	size_t ret = 0;

	ret = manager->alloc_ocssd_resource(vssd, request);

	if (!ret) {
		printf("No resource to allocate.\n");
		delete vssd;
		delete request;
		return -1;
	}

	if (request->get_remote()) {
		printf("Remote VSSD request.\n");
		remote_vssd_ = 1;
		vssd_ = vssd;
	}

	size_t len = vssd->serialize(buffer);

	int sent = send(connfd_, buffer, len, 0);
	printf("Alloc %lu channels, len %lu, sent %d\n", ret, len, sent);

	vssd->print();
	manager->persist();
	publish_resource();

	if (remote_vssd_ == 0)
		delete vssd;

	delete request;
	return 0;
}

bool ocssd_conn::process_write(REQUEST_CODE code)
{
	return false;
}

int ocssd_conn::epollfd = -1;
ocssd_manager *ocssd_conn::manager = NULL;

#endif
