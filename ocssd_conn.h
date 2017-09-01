#ifndef OCSSD_CONN_H
#define OCSSD_CONN_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <deque>
#include <exception>

#include "azure_config.h"
#include "ocssd_server.h"

#define MESSAGE_BUFFER_SIZE 24
#define VSSD_BUFFER_SIZE 1024
#define DATA_BUFFER_SIZE (16 * 1024 * 1024)

/**
 * In event based programming we need to queue up data to be written
 * until we are told by libevent that we can write.
 */
struct bufferq {
	bufferq(size_t size) : len(size), offset(0) {
		buf = (char *)malloc(size);
	}

	~bufferq() {free(buf);}

	/* The buffer. */
	char *buf;

	/* The length of buf. */
	size_t len;

	/* The offset into buf to start writing from. */
	size_t offset;
};

enum conn_state {
	RECEIVING_COMMAND = 0,
	RECEIVING_WRITE_DATA,
};

class ocssd_conn {
public:
	ocssd_conn(ocssd_manager *manager, int connfd,
			const struct sockaddr_in &client);
	~ocssd_conn();

	int process_incoming_requests(int fd);

	/* Events. We need 2 event structures, one for read event
	 * notification and the other for writing. */
	struct event ev_read;
	struct event ev_write;

	/* This is the queue of data to be written to this client. As
	 * we can't call write(2) until libevent tells us the socket
	 * is ready for writing. */
	std::deque<bufferq *> writeq;

private:
	ocssd_conn(const ocssd_conn &);
	ocssd_conn & operator=(const ocssd_conn &);

	int process_command(int fd);
	int process_write_data(int fd);

	int process_alloc_request(int fd);
	int process_read_request(int fd);
	int process_write_request(int fd);
	int process_erase_request(int fd);
	int publish_resource();
	int initialize_remote_vssd(virtual_ocssd *vssd);
	int initialize_vssd_blocks(const virtual_ocssd_unit * vunit);
	struct nvm_vblk *GetBlockPointer(size_t blk_idx) {return blks_array_[blk_idx];}

	ocssd_manager *manager;
	std::mutex mutex_;
	int connfd_;
	std::string ipaddr_;
	int message_start_;
	int message_end_;
	char message_buf_[MESSAGE_BUFFER_SIZE];
	conn_state state;

	/* Keep a virtual ssd here for remote access */
	int remote_vssd_;
	struct nvm_dev *dev_;
	const struct nvm_geo *geo_;

	size_t num_blks_;
	size_t blk_size_;
	std::vector<struct nvm_vblk *> blks_array_;		/* Real blocks */
};

ocssd_conn::ocssd_conn(ocssd_manager *manager, int connfd,
			const struct sockaddr_in &client)
	: manager(manager), connfd_(connfd), ipaddr_(inet_ntoa(client.sin_addr)),
	message_start_(0), message_end_(0), state(RECEIVING_COMMAND),
	remote_vssd_(0), dev_(NULL)
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
	}
}

int ocssd_conn::process_write_data(int fd)
{
	return 0;
}

int ocssd_conn::process_incoming_requests(int fd)
{
	int ret = 0;

	if (state == RECEIVING_COMMAND)
		ret = process_command(fd);
	else
		ret = process_write_data(fd);

	return ret;
}

int ocssd_conn::process_command(int fd)
{
	while (message_end_ < MESSAGE_BUFFER_SIZE) {
		int len = read(fd, message_buf_ + message_end_,
				MESSAGE_BUFFER_SIZE - message_end_);

		if (len == 0) {
			/* Client disconnected, remove the read event and the
			 * free the client structure. */
			printf("Client disconnected.\n");
			close(connfd_);
			event_del(&ev_read);
			return -1;
		} else if (len < 0) {
			/* Some other error occurred, close the socket, remove
			 * the event and free the client structure. */
			printf("Socket failure, disconnecting client: %s",
			    strerror(errno));
			close(connfd_);
			event_del(&ev_read);
			return -1;
		}

		message_end_ += len;
	}

	message_end_ = 0;
	int ret = 0;

	uint32_t header = get_header(message_buf_);

	switch (header) {
	case REQUEST_MAGIC:
		ret = process_alloc_request(fd);
		break;
	case READ_BLOCK_MAGIC:
		ret = process_read_request(fd);
		break;
	case WRITE_BLOCK_MAGIC:
		ret = process_write_request(fd);
		break;
	case ERASE_BLOCK_MAGIC:
		ret = process_erase_request(fd);
		break;
	default:
		return -1;
	}

	return ret;
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

static ssize_t nvm_vblk_test(const struct nvm_geo *geo, struct nvm_vblk *blk)
{
	size_t blk_size = nvm_vblk_get_nbytes(blk);
	size_t req_size = 262144;
	ssize_t ret = 0;
	int count = blk_size / req_size;
	void *buf;

	ret = nvm_vblk_erase(blk);
	if (ret)
		return ret;

	buf = nvm_buf_alloc(geo, req_size);
	for (int i = 0; i < count; i++) {
		ret = nvm_vblk_write(blk, buf, req_size);
		if (ret < 0)
			goto out;
	}

	for (int i = 0; i < count; i++) {
		ret = nvm_vblk_read(blk, buf, req_size);
		if (ret < 0)
			goto out;
	}

	ret = 0;
out:
	free(buf);
	return ret;
}

int ocssd_conn::initialize_vssd_blocks(const virtual_ocssd_unit * vunit)
{
	std::vector<uint32_t> curr_blocks;
	int count = 0;
	int i = 0;

	vunit->generate_units(curr_blocks);
	std::cout << __func__ << ": OCSSD blocks " << curr_blocks.size() << std::endl;

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

		if (nvm_vblk_test(geo_, blk) < 0) {
			std::cout << "Test block " << count << " failed" << std::endl;
			nvm_vblk_free(blk);
			continue;
		}

		if (!blk) {
			std::cout << __func__ << "FAILED: nvm_vblk_alloc" << std::endl;
			for (auto &vblk : blks_array_)
				nvm_vblk_free(vblk);
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


int ocssd_conn::process_alloc_request(int fd)
{
	ocssd_alloc_request request(message_buf_);
	virtual_ocssd *vssd = new virtual_ocssd();
	size_t ret = 0;

	ret = manager->alloc_ocssd_resource(vssd, &request);

	if (!ret || vssd->get_num_units() < 1) {
		printf("No resource to allocate.\n");
		delete vssd;
		return -1;
	}

	bufferq *bufferq = new class bufferq(VSSD_BUFFER_SIZE);
	if (!bufferq) {
		printf("No resource to allocate.\n");
		delete vssd;
		return -1;
	}

	if (request.get_remote()) {
		printf("Remote VSSD request.\n");
		remote_vssd_ = 1;
		initialize_remote_vssd(vssd);
	}

	size_t len = vssd->serialize(bufferq->buf);

	bufferq->len = len;
	writeq.push_back(bufferq);

	/* Since we now have data that needs to be written back to the
	 * client, add a write event. */
	event_add(&ev_write, NULL);

	vssd->print();
	manager->persist();
	publish_resource();

	delete vssd;
	return 0;
}

int ocssd_conn::process_read_request(int fd)
{
	ocssd_io_request request(message_buf_);
	uint32_t idx = request.get_block_index();
	size_t count = request.get_count();
	size_t offset = request.get_offset();
	ssize_t ret = 0;

//	printf("%s: block %u, size %lu, offset%lu\n", __func__, idx, count, offset);

	struct nvm_vblk *blk = GetBlockPointer(idx);
	if (!blk) {
		//FIXME: send error
		return -1;
	}

	bufferq *bufferq = new class bufferq(count);
	if (!bufferq) {
		printf("No resource to allocate.\n");
		return -1;
	}

	ret = nvm_vblk_pread(blk, bufferq->buf, count, offset);

	if (ret < 0) {
		printf("%s: read %ld, errno %d\n", __func__, ret, errno);
		printf("%s: block %u, size %lu, offset %lu\n", __func__, idx, count, offset);
		printf("pos write %lu\n", nvm_vblk_get_pos_write(blk));
		nvm_vblk_pr(blk);
	}

	writeq.push_back(bufferq);

	event_add(&ev_write, NULL);

	return 0;
}

int ocssd_conn::process_write_request(int fd)
{
	ocssd_io_request request(message_buf_);
	uint32_t idx = request.get_block_index();
	size_t count = request.get_count();
	size_t received = 0;
	ssize_t ret = 0;

//	printf("%s: block %u, size %lu\n", __func__, idx, count);

	struct nvm_vblk *blk = GetBlockPointer(idx);
	if (!blk) {
		//FIXME: send error
		return -1;
	}

	bufferq *bufferq = new class bufferq(count);
	if (!bufferq) {
		printf("No resource to allocate.\n");
		return -1;
	}

	while (received < count) {
		int len = read(fd, bufferq->buf + received, count - received);

		if (len == 0) {
			/* Client disconnected, remove the read event and the
			 * free the client structure. */
			printf("Client disconnected.\n");
			close(connfd_);
			event_del(&ev_read);
			return -1;
		} else if (len < 0) {
			/* Some other error occurred, close the socket, remove
			 * the event and free the client structure. */
			printf("Socket failure, disconnecting client: %s",
			    strerror(errno));
			close(connfd_);
			event_del(&ev_read);
			return -1;
		}

		received += len;
//		printf("%s: received %lu\n", __func__, received);
	}

	ret = nvm_vblk_write(blk, bufferq->buf, received);
	if (ret < 0) {
		printf("%s: written %ld, errno %d\n", __func__, ret, errno);
		printf("%s: block %u, size %lu\n", __func__, idx, count);
		printf("pos write %lu\n", nvm_vblk_get_pos_write(blk));
		nvm_vblk_pr(blk);
	}

	delete bufferq;
	return 0;
}

int ocssd_conn::process_erase_request(int fd)
{
	ocssd_io_request request(message_buf_);
	uint32_t idx = request.get_block_index();

	printf("Request block %u\n", idx);

	struct nvm_vblk *blk = GetBlockPointer(idx);
	if (!blk) {
		//FIXME: send error
		return -1;
	}

	nvm_vblk_erase(blk);

	return 0;
}

#endif
