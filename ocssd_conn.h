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

#define MESSAGE_BUFFER_SIZE 24
#define DATA_BUFFER_SIZE (16 * 1024 * 1024)


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
	int process_read_request(char *buffer);
	int process_write_request(char *buffer);
	int process_erase_request(char *buffer);
	int publish_resource();
	int initialize_remote_vssd(virtual_ocssd *vssd);
	int initialize_vssd_blocks(const virtual_ocssd_unit * vunit);
	struct nvm_vblk *GetBlockPointer(size_t blk_idx) {return blks_array_[blk_idx];}

	std::mutex mutex_;
	int connfd_;
	std::string ipaddr_;
	int message_start_;
	int message_end_;
	char message_buf_[MESSAGE_BUFFER_SIZE];

	/* Keep a virtual ssd here for remote access */
	int remote_vssd_;
	struct nvm_dev *dev_;
	const struct nvm_geo *geo_;

	size_t num_blks_;
	size_t blk_size_;
	std::vector<struct nvm_vblk *> blks_array_;		/* Real blocks */
	char* data_buf_;
};

ocssd_conn::ocssd_conn(int connfd, const struct sockaddr_in &client)
	: connfd_(connfd), ipaddr_(inet_ntoa(client.sin_addr)),
	message_start_(0), message_end_(0), remote_vssd_(0), dev_(NULL), data_buf_(NULL)
{
	std::cout << "New connection: conn " << connfd_
		<< ", IP addr " << ipaddr_ << std::endl;
}

ocssd_conn::~ocssd_conn()
{
	printf("%s\n", __func__);
	if (remote_vssd_) {
		nvm_dev_close(dev_);
		for (auto &vblk : blks_array_)
			nvm_vblk_free(vblk);
		free(data_buf_);
	}
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

	modfd(epollfd, connfd_, EPOLLOUT | EPOLLIN);
}

bool ocssd_conn::read()
{
	printf("read: %d\n", message_end_);
	MutexLock lock(&mutex_);
	if (message_end_ >= MESSAGE_BUFFER_SIZE)
		return true;

	int bytes_read = 0;
	while (true) {
		bytes_read = recv(connfd_, message_buf_ + message_end_,
					MESSAGE_BUFFER_SIZE - message_end_, 0);
		if (bytes_read == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return false;
		} else if (bytes_read == 0) {
			return false;
		}

		message_end_ += bytes_read;
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
		message_start_ += REQUEST_ALLOC_SIZE;
		if (message_start_ == message_end_)
			message_start_ = message_end_ = 0;
		break;
	case READ_BLOCK_MAGIC:
		if (size < REQUEST_IO_SIZE)
			break;
		ret = READ_BLOCK_REQUEST;
		message_start_ += REQUEST_IO_SIZE;
		if (message_start_ == message_end_)
			message_start_ = message_end_ = 0;
		break;
	case WRITE_BLOCK_MAGIC:
		if (size < REQUEST_IO_SIZE)
			break;
		ret = WRITE_BLOCK_REQUEST;
		message_start_ += REQUEST_IO_SIZE;
		if (message_start_ == message_end_)
			message_start_ = message_end_ = 0;
		break;
	case ERASE_BLOCK_MAGIC:
		if (size < REQUEST_IO_SIZE)
			break;
		ret = ERASE_BLOCK_REQUEST;
		message_start_ += REQUEST_IO_SIZE;
		if (message_start_ == message_end_)
			message_start_ = message_end_ = 0;
		break;
	default:
		break;
	}

	return ret;
}

REQUEST_CODE ocssd_conn::process_read()
{
	char temp_buf[1024];
	REQUEST_CODE command = NO_REQUEST;

	{
		MutexLock lock(&mutex_);
		printf("Received %d bytes\n", message_end_ - message_start_);

		memcpy(temp_buf, message_buf_ + message_start_, message_end_ - message_start_);
		command = parse_buffer(temp_buf, message_end_ - message_start_);
	}

	switch (command) {
	case ALLOC_VSSD_REQUEST:
		process_alloc_request(temp_buf);
		break;
	case READ_BLOCK_REQUEST:
		process_read_request(temp_buf);
		break;
	case WRITE_BLOCK_REQUEST:
		process_write_request(temp_buf);
		break;
	case ERASE_BLOCK_REQUEST:
		process_erase_request(temp_buf);
		break;
	default:
		break;
	}

	return command;
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

int ocssd_conn::initialize_vssd_blocks(const virtual_ocssd_unit * vunit)
{
	std::vector<uint32_t> curr_blocks;
	int count = 0;
	int i = 0;

	vunit->generate_units(curr_blocks);
	std::cout << __func__ << ": OCSSD blocks " << curr_blocks.size() << std::endl;

	data_buf_ = (char *)malloc(DATA_BUFFER_SIZE);
	if (!data_buf_)
		return -ENOMEM;

	while (true) {
		std::vector<struct ::nvm_addr> addrs;
		i = 0;

		for (const virtual_ocssd_channel *vchannel : vunit->get_channels()) {
			uint32_t channel_id = vchannel->get_channel_id();
			uint32_t num_luns = vchannel->get_num_luns();
			uint32_t lun_id;
			uint32_t block_end;

			if (vchannel->is_shared()) {
				for (const virtual_ocssd_lun * lun : vchannel->get_luns()) {
					lun_id = lun->get_lun_id();
					block_end = lun->get_block_start() + lun->get_num_blocks();

					if (curr_blocks[i] < block_end)
						generate_addr(addrs, curr_blocks, channel_id, lun_id, i);
					i++;
				}
			} else {
				for (lun_id = 0; lun_id < num_luns; lun_id++) {
					block_end = geo_->nblocks;

					if (curr_blocks[i] < block_end)
						generate_addr(addrs, curr_blocks, channel_id, lun_id, i);
					i++;
				}
			}
		}

		if (addrs.size() == 0)
			break;

		struct nvm_vblk *blk;

		blk = nvm_vblk_alloc(dev_, addrs.data(), addrs.size());

		if (!blk) {
			std::cout << __func__ << "FAILED: nvm_vblk_alloc" << std::endl;
			for (auto &vblk : blks_array_)
				nvm_vblk_free(vblk);
			free(data_buf_);
			return -ENOMEM;
		}

		blks_array_.push_back(blk);
		/* FIXME: Assume each block has equal size */
		blk_size_ = nvm_vblk_get_nbytes(blk);
		count++;
	}

	std::cout << __func__ << ": " << count << " vblks" << std::endl;
	num_blks_ = count;

	// FIXME: Restore vblk states

	return 0;
}

int ocssd_conn::initialize_remote_vssd(virtual_ocssd *vssd)
{
	//FIXME: Use only one unit now
	const virtual_ocssd_unit *vunit = vssd->get_unit(0);

	std::string dev_path = vunit->get_dev_name();
	dev_ = nvm_dev_open(dev_path.c_str());
	if (!dev_) {
		std::cout << __func__ << ": FAILED: opening device" << std::endl;
		throw std::runtime_error("FAILED: opening device");
		return -EIO;
	}

	geo_ = nvm_dev_get_geo(dev_);

	initialize_vssd_blocks(vunit);

	std::cout << __func__ << ": " << dev_path << std::endl;

	return 0;
}


int ocssd_conn::process_alloc_request(char *buffer)
{
	ocssd_alloc_request request(buffer);
	virtual_ocssd *vssd = new virtual_ocssd();
	size_t ret = 0;

	MutexLock lock(&mutex_);

	ret = manager->alloc_ocssd_resource(vssd, &request);

	if (!ret || vssd->get_num_units() < 1) {
		printf("No resource to allocate.\n");
		delete vssd;
		return -1;
	}

	if (request.get_remote()) {
		printf("Remote VSSD request.\n");
		remote_vssd_ = 1;
		initialize_remote_vssd(vssd);
	}

	size_t len = vssd->serialize(buffer);

	int sent = send(connfd_, buffer, len, 0);
	printf("Alloc %lu channels, len %lu, sent %d\n", ret, len, sent);

	vssd->print();
	manager->persist();
	publish_resource();

	delete vssd;
	return 0;
}

int ocssd_conn::process_read_request(char *buffer)
{
	ocssd_io_request request(buffer);
	uint32_t idx = request.get_block_index();
	size_t count = request.get_count();
	size_t offset = request.get_offset();
	size_t ret = 0;

	printf("%s: block %u, size %lu, offset%lu\n", __func__, idx, count, offset);

	MutexLock lock(&mutex_);

	struct nvm_vblk *blk = GetBlockPointer(idx);
	if (!blk) {
		//FIXME: send error
		return -1;
	}

	ret = nvm_vblk_pread(blk, data_buf_, count, offset);
	send(connfd_, data_buf_, ret, 0);

	return 0;
}

int ocssd_conn::process_write_request(char *buffer)
{
	ocssd_io_request request(buffer);
	uint32_t idx = request.get_block_index();
	size_t count = request.get_count();
	size_t received = 0;
	size_t ret = 0;

	printf("%s: block %u, size %lu\n", __func__, idx, count);

	MutexLock lock(&mutex_);

	struct nvm_vblk *blk = GetBlockPointer(idx);
	if (!blk) {
		//FIXME: send error
		return -1;
	}

	while ((ret = recv(connfd_, data_buf_ + received,
					count - received, 0)) > 0)
		received += ret;

	ret = nvm_vblk_write(blk, data_buf_, received);
	send(connfd_, data_buf_, ret, 0);

	return 0;
}

int ocssd_conn::process_erase_request(char *buffer)
{
	ocssd_io_request request(buffer);
	uint32_t idx = request.get_block_index();

	printf("%s: block %u\n", __func__, idx);

	MutexLock lock(&mutex_);

	struct nvm_vblk *blk = GetBlockPointer(idx);
	if (!blk) {
		//FIXME: send error
		return -1;
	}

	nvm_vblk_erase(blk);

	return 0;
}

bool ocssd_conn::process_write(REQUEST_CODE code)
{
	return true;
}

int ocssd_conn::epollfd = -1;
ocssd_manager *ocssd_conn::manager = NULL;

#endif
