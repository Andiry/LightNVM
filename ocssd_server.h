#pragma once

#include <liblightnvm.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>

#define OCSSD_MESSAGE_PORT	50001
#define OCSSD_DATA_PORT		50002

#define MESSAGE_BUFFER_SIZE 24

static int setnonblocking(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	if (one_shot)
		event.events |= EPOLLONESHOT;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
	setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = ev | EPOLLONESHOT | EPOLLET | EPOLLRDHUP;
	epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

std::string get_ip()
{
	struct ifaddrs *ifAddrStruct = NULL;
	struct ifaddrs *ifa = NULL;
	void * tmpAddrPtr = NULL;
	std::string prefix("eno1");
	std::string ret;

	getifaddrs(&ifAddrStruct);

	for (ifa = ifAddrStruct; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr)
			continue;

		/* Only get IPv4 addr */
		if (ifa->ifa_addr->sa_family == AF_INET) {
			char addressBuffer[INET_ADDRSTRLEN];
			tmpAddrPtr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
			inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
			if (!prefix.compare(0, prefix.size(), ifa->ifa_name)) {
				ret = addressBuffer;
				break;
			}
		}
	}

	if (ifAddrStruct)
		freeifaddrs(ifAddrStruct);

	return ret;
}

void generate_addr(std::vector<struct nvm_addr> &addrs,
	std::vector<uint32_t> &curr_blocks,
	uint32_t channel_id,
	uint32_t lun_id,
	int i)
{
	struct nvm_addr addr;

	addr.ppa = 0;
	addr.g.ch = channel_id;
	addr.g.lun = lun_id;
	addr.g.blk = curr_blocks[i];
	curr_blocks[i]++;
	addrs.push_back(addr);
}

ssize_t nvm_vblk_test(const struct nvm_geo *geo, struct nvm_vblk *blk)
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

class MutexLock {
public:
	explicit MutexLock(std::mutex *mutex)
	: mutex_(mutex) {
		mutex_->lock();
	}

	~MutexLock() { mutex_->unlock();}

private:
	std::mutex *const mutex_;
	// No copying
	MutexLock(const MutexLock&);
	void operator=(const MutexLock&);
};


static inline uint32_t get_header(const char *buffer) {
	uint32_t data;
	data = *(uint32_t *)buffer;
	return data;
}

static inline void serialize_data4(char *&buffer, uint32_t data) {
	char *&p = buffer;
	*(uint32_t *)p = data;
	p += sizeof(uint32_t);
}

static inline uint32_t deserialize_data4(const char *&buffer) {
	uint32_t data;
	const char *&p = buffer;
	data = *(uint32_t *)p;
	p += sizeof(uint32_t);
	return data;
}

static inline void serialize_data8(char *&buffer, uint64_t data) {
	char *&p = buffer;
	*(uint64_t *)p = data;
	p += sizeof(uint64_t);
}

static inline uint64_t deserialize_data8(const char *&buffer) {
	uint64_t data;
	const char *&p = buffer;
	data = *(uint64_t *)p;
	p += sizeof(uint64_t);
	return data;
}


/* ====================== OCSSD structs ======================== */

enum REQUEST_CODE {
	NO_REQUEST = 0,
	ALLOC_VSSD_REQUEST,
	READ_BLOCK_REQUEST,
	WRITE_BLOCK_REQUEST,
	ERASE_BLOCK_REQUEST,
};

const uint32_t READ_BLOCK_MAGIC = 0x6401;
const uint32_t WRITE_BLOCK_MAGIC = 0x6402;
const uint32_t ERASE_BLOCK_MAGIC = 0x6403;
const ssize_t REQUEST_IO_SIZE = 24;

/*
 * Request serialize format:
 * BLOCK_MAGIC		4 bytes
 * BLOCK_INDEX		4 bytes
 * COUNT		8 bytes
 * OFFSET		8 bytes
 */
class ocssd_io_request {
public:

	ocssd_io_request(REQUEST_CODE command, uint32_t block_index, size_t count, size_t offset)
		: command_(command),
		block_index_(block_index),
		count_(count),
		offset_(offset) {}

	ocssd_io_request(const char *buffer) {
		uint32_t magic = deserialize_data4(buffer);
		if (magic != READ_BLOCK_MAGIC &&
				magic != WRITE_BLOCK_MAGIC &&
				magic != ERASE_BLOCK_MAGIC) {
			printf("Incorrect MAGIC: %x\n", magic);
			throw std::runtime_error("Error: init request failed\n");
		}

		switch (magic) {
		case (READ_BLOCK_MAGIC):
			command_ = READ_BLOCK_REQUEST;
			break;
		case (WRITE_BLOCK_MAGIC):
			command_ = WRITE_BLOCK_REQUEST;
			break;
		case (ERASE_BLOCK_MAGIC):
			command_ = ERASE_BLOCK_REQUEST;
			break;
		default:
			break;
		}

		block_index_	= deserialize_data4(buffer);
		count_		= deserialize_data8(buffer);
		offset_		= deserialize_data8(buffer);

//		printf("%d request: block %d, count %lu, offset %lu\n",
//			command_, block_index_, count_, offset_);
	}

	size_t serialize(char *buffer) {
		char *start = buffer;

		switch (command_) {
		case (READ_BLOCK_REQUEST):
			serialize_data4(buffer, READ_BLOCK_MAGIC);
			break;
		case (WRITE_BLOCK_REQUEST):
			serialize_data4(buffer, WRITE_BLOCK_MAGIC);
			break;
		case (ERASE_BLOCK_REQUEST):
			serialize_data4(buffer, ERASE_BLOCK_MAGIC);
			break;
		default:
			return 0;
		}

		serialize_data4(buffer, block_index_);
		serialize_data8(buffer, count_);
		serialize_data8(buffer, offset_);
		return buffer - start;
	}

	REQUEST_CODE get_command() {return command_;}
	uint32_t get_block_index() {return block_index_;}
	size_t get_count() {return count_;}
	size_t get_offset() {return offset_;}

private:

	REQUEST_CODE command_;
	uint32_t block_index_;
	size_t count_;
	size_t offset_;
};

const uint32_t REQUEST_MAGIC = 0x6501;
const ssize_t REQUEST_ALLOC_SIZE = 24;

/*
 * Request serialize format:
 * REQUEST_MAGIC	4 bytes
 * NUM_CHANNELS		4 bytes
 * NUM_BLOCKS		4 bytes
 * SHARED		4 bytes
 * NUMA_ID		4 bytes
 * REMOTE		4 bytes
 */
class ocssd_alloc_request {
public:

	ocssd_alloc_request(size_t num_channels, size_t num_blocks, int shared, int numa_id, int remote)
		: num_channels_(num_channels),
		num_blocks_(num_blocks),
		shared_(shared),
		numa_id_(numa_id),
		remote_(remote) {}

	ocssd_alloc_request(size_t num_channels, int numa_id, int remote)
		: num_channels_(num_channels),
		num_blocks_(0),
		shared_(0),
		numa_id_(numa_id),
		remote_(remote) {}

	ocssd_alloc_request(const char *buffer) {
		uint32_t magic = deserialize_data4(buffer);
		if (magic != REQUEST_MAGIC) {
			printf("Incorrect MAGIC: %x\n", magic);
			throw std::runtime_error("Error: init request failed\n");
		}

		num_channels_	= deserialize_data4(buffer);
		num_blocks_	= deserialize_data4(buffer);
		shared_		= deserialize_data4(buffer);
		numa_id_	= deserialize_data4(buffer);
		remote_		= deserialize_data4(buffer);

		printf("Request %u channels, %u blocks, shared %u, NUMA %u, remote %u\n",
			num_channels_, num_blocks_, shared_, numa_id_, remote_);
	}

	size_t serialize(char *buffer) {
		char *start = buffer;
		serialize_data4(buffer, REQUEST_MAGIC);
		serialize_data4(buffer, num_channels_);
		serialize_data4(buffer, num_blocks_);
		serialize_data4(buffer, shared_);
		serialize_data4(buffer, numa_id_);
		serialize_data4(buffer, remote_);
		return buffer - start;
	}

	uint32_t get_channels() {return num_channels_;}
	uint32_t get_blocks() {return num_blocks_;}
	void dec_channels(size_t channels) {num_channels_ -= channels;}
	uint32_t get_shared() {return shared_;}
	uint32_t get_numa_id() {return numa_id_;}
	uint32_t get_remote() {return remote_;}

private:

	uint32_t num_channels_;
	uint32_t num_blocks_;
	uint32_t shared_;
	uint32_t numa_id_;
	uint32_t remote_;
};

/*
 * Virtual OCSSD serialization format:
 * SERIALIZE_MAGIC		4 bytes
 * VIRTUAL_SSD_ID		4 bytes
 * NUM_UNITS			4 bytes
 *	DEV_NAME_LEN		4 bytes
 *	DEV_NAME		4 bytes * N
 *	GEO_NCHANNELS		8 bytes
 *	GEO_NLUNS		8 bytes
 *	GEO_NPLANCES		8 bytes
 *	GEO_NBLOCKS		8 bytes
 *	GEO_NPAGES		8 bytes
 *	GEO_NSECTORS		8 bytes
 *	GEO_PAGE_NBYTES		8 bytes
 *	GEO_SECTOR_NBYTES	8 bytes
 *	GEO_META_NBYTES		8 bytes
 *	NUM_CHANNELS		4 bytes
 *		CHANNEL_ID	4 bytes
 *		SHARED		4 bytes
 *		TOTAL_BLOCKS	4 bytes		Total blocks in this channel
 *		NUM_LUNS	4 bytes
 *		LUN_ID		4 bytes		if SHARED == 1
 *		BLOCKS_START	4 bytes
 *		BLOCKS_NUM	4 bytes
 *		LUN_ID		4 bytes		if SHARED == 1
 *		...
 */

class virtual_ocssd_lun {
public:
	virtual_ocssd_lun(uint32_t lun_id,
		uint32_t block_start,
		uint32_t num_blocks)
		: lun_id_(lun_id),
		block_start_(block_start),
		num_blocks_(num_blocks) {}

	virtual_ocssd_lun(const char *&buffer);

	uint32_t get_lun_id() const { return lun_id_;}
	uint32_t get_block_start() const { return block_start_;}
	uint32_t get_num_blocks() const { return num_blocks_;}

	size_t serialize(char *&buffer) const;
	void print() const;

private:
	uint32_t lun_id_;
	uint32_t block_start_;
	uint32_t num_blocks_;
};

size_t virtual_ocssd_lun::serialize(char *&buffer) const {
	char *start = buffer;
	char *&p = buffer;

	serialize_data4(p, lun_id_);
	serialize_data4(p, block_start_);
	serialize_data4(p, num_blocks_);

	return p - start;
}

virtual_ocssd_lun::virtual_ocssd_lun(const char *&buffer) {
	const char *&p = buffer;

	lun_id_ = deserialize_data4(p);
	block_start_ = deserialize_data4(p);
	num_blocks_ = deserialize_data4(p);

	std::cout << "LUN " << lun_id_ << ": "
		  << "block start " << block_start_ << ", "
		  << num_blocks_ << " blocks" << std::endl;
}

void virtual_ocssd_lun::print() const {
	std::cout << "LUN " << lun_id_ << ": "
		  << "block start " << block_start_ << ", "
		  << num_blocks_ << " blocks" << std::endl;
}

class virtual_ocssd_channel {
public:

	virtual_ocssd_channel(uint32_t channel_id = 0,
		uint32_t shared = 0,
		uint32_t total_blocks = 0,
		uint32_t num_luns = 0)
		: channel_id_(channel_id),
		shared_(shared),
		total_blocks_(total_blocks),
		num_luns_(num_luns) {}

	virtual_ocssd_channel(uint32_t channel_id,
		uint32_t shared,
		const std::vector<std::pair<size_t, std::pair<size_t, size_t>>>& alloc_units)
		: channel_id_(channel_id),
		shared_(shared),
		total_blocks_(0),
		num_luns_(0)
	{
		for (auto it : alloc_units) {
			virtual_ocssd_lun * vlun = new virtual_ocssd_lun(it.first,
							it.second.first,
							it.second.second);
			luns_.push_back(vlun);
			total_blocks_ += vlun->get_num_blocks();
			num_luns_++;
		}
	}

	~virtual_ocssd_channel() {
		for (virtual_ocssd_lun * vlun : luns_)
			delete vlun;
	}

	void generate_units(std::vector<uint32_t> &units);
	size_t serialize(char *&buffer) const;
	size_t deserialize(const char *&buffer);

	void add(virtual_ocssd_lun * lun) {
		luns_.push_back(lun);
	}

	const uint32_t get_channel_id() const {return channel_id_;}
	bool is_shared() const {return shared_ > 0;}
	const uint32_t get_total_blocks() const {return total_blocks_;}
	const uint32_t get_num_luns() const {return num_luns_;}
	const std::vector<virtual_ocssd_lun *>& get_luns() const {return luns_;}
	void print() const;

private:
	uint32_t channel_id_;
	uint32_t shared_;
	uint32_t total_blocks_;
	uint32_t num_luns_;
	std::vector<virtual_ocssd_lun *> luns_;
};

void virtual_ocssd_channel::generate_units(std::vector<uint32_t> &units)
{
	if (shared_ == 1) {
		for (virtual_ocssd_lun * lun : luns_)
			units.push_back(lun->get_block_start());
	} else {
		// Use all blocks start at 0
		for (uint32_t lun = 0; lun < num_luns_; lun++)
			units.push_back(0);
	}
}

size_t virtual_ocssd_channel::serialize(char *&buffer) const {
	char *start = buffer;
	char *&p = buffer;

	serialize_data4(p, channel_id_);
	serialize_data4(p, shared_);
	serialize_data4(p, total_blocks_);
	serialize_data4(p, num_luns_);

	if (shared_ == 1) {
		for (const virtual_ocssd_lun * lun : luns_)
			lun->serialize(p);
	}

	return p - start;
}

size_t virtual_ocssd_channel::deserialize(const char *&buffer) {
	const char *start = buffer;
	const char *&p = buffer;

	channel_id_ = deserialize_data4(p);
	shared_ = deserialize_data4(p);
	total_blocks_ = deserialize_data4(p);
	num_luns_ = deserialize_data4(p);

	std::cout << "Channel " << channel_id_ << ": "
		  << "shared " << shared_ << ", "
		  << num_luns_ << " LUNs, "
		  << total_blocks_ << " blocks" << std::endl;

	if (shared_ == 1) {
		for (uint32_t i = 0; i < num_luns_; i++)
			luns_.push_back(new virtual_ocssd_lun(p));
	}

	return p - start;
}

void virtual_ocssd_channel::print() const {
	std::cout << "Channel " << channel_id_ << ": "
		  << "shared " << shared_ << ", "
		  << num_luns_ << " LUNs, total "
		  << total_blocks_ << " blocks" << std::endl;
	for (virtual_ocssd_lun * vlun : luns_)
		vlun->print();
}

class virtual_ocssd_unit {
public:

	virtual_ocssd_unit(std::string dev_name, const struct nvm_geo *geo)
			:dev_name_(dev_name) {
		geo_ = new nvm_geo;
		memcpy(geo_, geo, sizeof(struct nvm_geo));
	}

	virtual_ocssd_unit() {geo_ = new nvm_geo;}

	~virtual_ocssd_unit() {
		for (auto channel : channels_)
			delete channel;
		delete geo_;
	}

	void generate_units(std::vector<uint32_t> &units) const;
	size_t serialize(char *&buffer) const;
	size_t serialize_geo(char *&buffer) const;
	size_t deserialize(const char *&buffer);
	size_t deserialize_geo(const char *&buffer);
	void add(virtual_ocssd_channel *channel) {channels_.push_back(channel);}

	const std::string& get_dev_name() const {return dev_name_;}
	const struct nvm_geo * get_geo() const {return geo_;}
	const std::vector<virtual_ocssd_channel *>& get_channels() const {return channels_;}
	void print() const;

private:
	std::string dev_name_;
	struct nvm_geo *geo_;
	std::vector<virtual_ocssd_channel *> channels_;
};

void virtual_ocssd_unit::generate_units(std::vector<uint32_t> &units) const
{
	for (virtual_ocssd_channel *channel : channels_)
		channel->generate_units(units);
}

size_t virtual_ocssd_unit::serialize_geo(char *&buffer) const
{
	char *start = buffer;
	char *&p = buffer;

	serialize_data8(p, geo_->nchannels);
	serialize_data8(p, geo_->nluns);
	serialize_data8(p, geo_->nplanes);
	serialize_data8(p, geo_->nblocks);
	serialize_data8(p, geo_->npages);
	serialize_data8(p, geo_->nsectors);
	serialize_data8(p, geo_->page_nbytes);
	serialize_data8(p, geo_->sector_nbytes);
	serialize_data8(p, geo_->meta_nbytes);

	return p - start;
}

size_t virtual_ocssd_unit::serialize(char *&buffer) const
{
	char *start = buffer;
	char *&p = buffer;
	size_t len = dev_name_.length() + 1;

	serialize_data4(p, len);
	memcpy(p, dev_name_.c_str(), len);
	len = (len + 3) / 4 * 4;
	p += len;

	serialize_geo(p);

	serialize_data4(p, channels_.size());

	for (const virtual_ocssd_channel *channel : channels_)
		channel->serialize(p);

	return p - start;
}

size_t virtual_ocssd_unit::deserialize_geo(const char *&buffer)
{
	const char *start = buffer;
	const char *&p = buffer;

	geo_->nchannels		= deserialize_data8(p);
	geo_->nluns		= deserialize_data8(p);
	geo_->nplanes		= deserialize_data8(p);
	geo_->nblocks		= deserialize_data8(p);
	geo_->npages		= deserialize_data8(p);
	geo_->nsectors		= deserialize_data8(p);
	geo_->page_nbytes	= deserialize_data8(p);
	geo_->sector_nbytes	= deserialize_data8(p);
	geo_->meta_nbytes	= deserialize_data8(p);

	return p - start;
}

size_t virtual_ocssd_unit::deserialize(const char *&buffer)
{
	const char *start = buffer;
	const char *&p = buffer;
	uint32_t name_len = deserialize_data4(p);
	const char *temp = p;

	dev_name_ = temp;
	name_len = (name_len + 3) / 4 * 4;
	p += name_len;

	deserialize_geo(p);

	uint32_t num_channel = deserialize_data4(p);
	std::cout<< "Device " << dev_name_ << ": "
		 << num_channel << " channels" << std::endl;

	for (uint32_t i = 0; i < num_channel; i++) {
		virtual_ocssd_channel *channel = new virtual_ocssd_channel();
		channel->deserialize(p);
		channels_.push_back(channel);
	}

	return p - start;
}

void virtual_ocssd_unit::print() const
{
	std::cout<< "Device " << dev_name_ << ": "
		 << channels_.size() << " channels" << std::endl;
	for (virtual_ocssd_channel * vchannel : channels_)
		vchannel->print();
}

class virtual_ocssd {
public:
	virtual_ocssd() : id_(0) {}
	~virtual_ocssd() {
		for (auto p : units_)
			delete p;
	}

	size_t serialize(char *buffer) const;
	size_t deserialize(const char *buffer);
	void add(virtual_ocssd_unit *unit) {units_.push_back(unit);}
	void set_id(uint32_t id) {id_ = id;}
	uint32_t get_id() const { return id_;}

	size_t get_num_units() const {return units_.size();}
	const virtual_ocssd_unit *get_unit(int i) const {return units_[i];}
	void print() const;

private:
	uint32_t id_;
	std::vector<virtual_ocssd_unit *> units_;
};

const uint32_t SERIALIZE_MAGIC = 0x6502;

size_t virtual_ocssd::serialize(char *buffer) const {
	char *p = buffer;

	serialize_data4(p, SERIALIZE_MAGIC);
	serialize_data4(p, id_);
	serialize_data4(p, units_.size());

	for (const virtual_ocssd_unit *unit : units_)
		unit->serialize(p);

	return p - buffer;
}

size_t virtual_ocssd::deserialize(const char *buffer) {
	const char *p = buffer;
	uint32_t magic;
	uint32_t num_unit = 0;

	magic = deserialize_data4(p);
	if (magic != SERIALIZE_MAGIC) {
		printf("Incorrect MAGIC: %x\n", magic);
		return 0;
	}

	id_ = deserialize_data4(p);
	num_unit = deserialize_data4(p);
	printf("ID %u, %u Devices\n", id_, num_unit);

	for (uint32_t i = 0; i < num_unit; i++) {
		virtual_ocssd_unit *unit = new virtual_ocssd_unit();
		unit->deserialize(p);
		units_.push_back(unit);
	}

	return p - buffer;
}

void virtual_ocssd::print() const {
	printf("Virtual SSD ID %u, %lu Devices\n", id_, units_.size());
	for (virtual_ocssd_unit * vunit : units_)
		vunit->print();
}

/* ====================== Physical resource ======================== */

/* OCSSD LUN(Die) */
class ocssd_lun {

public:
	ocssd_lun(size_t lun_id, size_t num_blocks)
		:lun_id_(lun_id),
		num_used_(0),
		num_blocks_(num_blocks) {}

	~ocssd_lun() {}

	size_t get_lun_id() {
		return lun_id_;
	}

	size_t get_num_blocks() {
		return num_blocks_;
	}

	size_t get_num_used_blocks() {
		return num_used_;
	}

	size_t alloc_blocks(std::pair<size_t, size_t> & block_unit, size_t request_blocks) {
		if (num_used_ == num_blocks_ || request_blocks == 0)
			return 0;

		size_t allocated = std::min(request_blocks, num_blocks_ - num_used_);

		block_unit.first = num_used_;
		block_unit.second = allocated;
		num_used_ += allocated;

		return allocated;
	}

private:

	size_t lun_id_;
	size_t num_used_;
	size_t num_blocks_;
};

/* OCSSD channel */
class ocssd_channel {
public:

	ocssd_channel(size_t channel_id, size_t num_luns, size_t num_blocks)
		:channel_id_(channel_id),
		num_luns_(num_luns),
		num_blocks_(num_blocks),
		shared_(0),
		used_(0)
	{
		num_used_blocks_ = 0;
		num_total_blocks_ = num_blocks * num_luns;

		for (size_t lun_id = 0; lun_id < num_luns; lun_id++) {
			ocssd_lun * lun = new ocssd_lun(lun_id, num_blocks);
			luns_.push_back(lun);
		}
	}

	~ocssd_channel() {
		for (size_t lun_id = 0; lun_id < num_luns_; lun_id++)
			delete luns_[lun_id];
	}

	void set_shared() {
		shared_ = 1;
	}

	size_t used() {
		return used_;
	}

	void set_used() {
		used_ = 1;
	}

	size_t get_channel_id() {
		return channel_id_;
	}

	size_t get_total_blocks() {
		return num_total_blocks_;
	}

	size_t get_free_blocks() {
		if (used_)
			return 0;

		return num_total_blocks_ - num_used_blocks_;
	}

	size_t get_num_luns() {
		return num_luns_;
	}

	size_t alloc_blocks(std::vector<std::pair<size_t, std::pair<size_t, size_t>>> & alloc_units,
					size_t request_blocks) {
		if (num_used_blocks_ == num_total_blocks_ || request_blocks == 0)
			return 0;

		size_t allocated = 0;
		size_t request_per_lun = request_blocks / num_luns_;

		/* Distribute vblk across all the LUNs */
		for (ocssd_lun * lun : luns_) {
			if (lun->get_num_used_blocks() >= num_blocks_)
				continue;

			std::pair<size_t, size_t> block_units;
			size_t ret = lun->alloc_blocks(block_units, request_per_lun);

			alloc_units.push_back(std::pair<size_t, std::pair<size_t, size_t>>
						(lun->get_lun_id(), block_units));

			request_blocks -= ret;
			allocated += ret;
			if (request_blocks == 0)
				break;
		}

		num_used_blocks_ += allocated;
		return allocated;
	}

private:

	size_t channel_id_;
	size_t num_luns_;
	size_t num_blocks_;		/* # blocks per LUN */
	size_t num_used_blocks_;
	size_t num_total_blocks_;	/* Total blocks of this channel */
	int shared_;
	int used_;
	std::vector<ocssd_lun *> luns_;
};

/* Represents a physical OCSSD */
class ocssd_unit {
public:

	ocssd_unit(const std::string &ip, const std::string &name)
		: ip_(ip), name_(name), desc_(ip + name)
	{
		std::replace(desc_.begin(), desc_.end(), '/', '_');
		dev_ = nvm_dev_open(name.c_str());
		if (!dev_)
			throw std::runtime_error("Error: open dev failed\n");

		initialize_dev();
	}

	~ocssd_unit() {
		for (ocssd_channel *channel : shared_channels_)
			delete channel;

		for (ocssd_channel *channel : exclusive_channels_)
			delete channel;

		nvm_dev_close(dev_);
	}

	std::string get_name() {
		return name_;
	}

	const std::string & get_desc() {
		return desc_;
	}

	size_t alloc_channels(virtual_ocssd *vssd, ocssd_alloc_request *request);
	int get_ocssd_stats(
		size_t &numSharedChannels,
		size_t &numExclusiveChannels,
		size_t &freeBlocks);

private:

	int initialize_dev();
	int assign_to_shared(ocssd_channel *channel);

	size_t alloc_shared_channels(virtual_ocssd_unit *vunit,
		virtual_ocssd *vssd, ocssd_alloc_request *request);
	size_t alloc_exclusive_channels(virtual_ocssd_unit *vunit,
		virtual_ocssd *vssd, ocssd_alloc_request *request);

	std::string ip_;
	std::string name_;
	std::string desc_;
	struct nvm_dev *dev_;
	const struct nvm_geo *geo_;
	std::mutex mutex_;
	int channel_count_ = 0;
	std::vector<ocssd_channel *> shared_channels_;
	std::vector<ocssd_channel *> exclusive_channels_;
};

int ocssd_unit::assign_to_shared(ocssd_channel *channel)
{
	if (shared_channels_.size() < 4)
		return 1;

	return 0;
}

int ocssd_unit::initialize_dev()
{
	geo_ = nvm_dev_get_geo(dev_);
	printf("geo: %lu channels, %lu LUNs per channel, %lu blocks per LUN\n",
		geo_->nchannels, geo_->nluns, geo_->nblocks);

	for (size_t channel_id = 0; channel_id < geo_->nchannels; channel_id++) {
		ocssd_channel *channel = new ocssd_channel(channel_id,
							geo_->nluns,
							geo_->nblocks);

		if (assign_to_shared(channel)) {
			channel->set_shared();
			shared_channels_.push_back(channel);
		} else {
			exclusive_channels_.push_back(channel);
		}

		channel_count_++;
	}

	std::cout << "Get " << channel_count_ << " channels for " << name_ << std::endl;
	std::cout << "Share " << shared_channels_.size() << " channels, "
		  << "exclusive " << exclusive_channels_.size() << " channels" << std::endl;

	return 0;
}

size_t ocssd_unit::alloc_shared_channels(virtual_ocssd_unit *vunit,
	virtual_ocssd *vssd, ocssd_alloc_request *request)
{
	size_t channels = 0;
	/* FIXME: Distribute the request among channels */
	size_t blocks_per_channel = request->get_blocks() / request->get_channels();

	for (ocssd_channel *channel : shared_channels_) {
		std::vector<std::pair<size_t, std::pair<size_t, size_t>>> alloc_units;
		size_t allocated = channel->alloc_blocks(alloc_units, blocks_per_channel);
		if (allocated > 0) {
			virtual_ocssd_channel *vchannel =
				new virtual_ocssd_channel(channel->get_channel_id(), 1, alloc_units);

			vunit->add(vchannel);
			channels++;
			if (channels == request->get_channels())
				break;
		}
	}

	return channels;
}

size_t ocssd_unit::alloc_exclusive_channels(virtual_ocssd_unit *vunit,
	virtual_ocssd *vssd, ocssd_alloc_request *request)
{
	size_t channels = 0;

	for (ocssd_channel *channel : exclusive_channels_) {
		if (channel->used())
			continue;

		channel->set_used();
		virtual_ocssd_channel *vchannel =
			new virtual_ocssd_channel(channel->get_channel_id(), 0,
						channel->get_total_blocks(),
						channel->get_num_luns());

		vunit->add(vchannel);

		channels++;
		if (channels == request->get_channels())
			break;
	}

	return channels;
}

size_t ocssd_unit::alloc_channels(virtual_ocssd *vssd, ocssd_alloc_request *request)
{
	size_t channels = 0;

	if (request->get_channels() == 0)
		return 0;

	virtual_ocssd_unit *vunit = new virtual_ocssd_unit(name_, geo_);

	MutexLock lock(&mutex_);

	if (request->get_shared() == 1)
		channels = alloc_shared_channels(vunit, vssd, request);
	else
		channels = alloc_exclusive_channels(vunit, vssd, request);

	if (channels == 0)
		delete vunit;
	else
		vssd->add(vunit);

	return channels;
}

int ocssd_unit::get_ocssd_stats(
	size_t &numSharedChannels,
	size_t &numExclusiveChannels,
	size_t &freeBlocks)
{
	size_t shared = 0;
	size_t exclusive = 0;
	size_t blocks = 0;
	size_t free_blocks = 0;

	MutexLock lock(&mutex_);

	for (ocssd_channel *channel : shared_channels_) {
		free_blocks = channel->get_free_blocks();
		if (free_blocks > 0) {
			blocks += free_blocks;
			shared++;
		}
	}

	for (ocssd_channel *channel : exclusive_channels_) {
		free_blocks = channel->get_free_blocks();
		if (free_blocks > 0) {
			blocks += free_blocks;
			exclusive++;
		}
	}

	numSharedChannels = shared;
	numExclusiveChannels = exclusive;
	freeBlocks = blocks;

	return 0;
}

/* Represents all the OCSSDs on a single node */
class ocssd_manager {
public:

	ocssd_manager() : count_(0), vssd_id_(0) {
		ip_ = get_ip();
		std::cout << ip_ << std::endl;
	}

	~ocssd_manager() {
		for (auto unit : ocssds_)
			delete unit;
	}

	int add_ocssd(const std::string &name);
	size_t alloc_ocssd_resource(virtual_ocssd *vssd, ocssd_alloc_request *request);
	const std::vector<ocssd_unit *> & get_units();
	int persist() { return 0;}

private:

	std::string ip_;
	std::mutex mutex_;
	std::vector<ocssd_unit *> ocssds_;
	int count_;
	uint32_t vssd_id_;
};

int ocssd_manager::add_ocssd(const std::string &name)
{
	MutexLock lock(&mutex_);
	ocssd_unit *unit = new ocssd_unit(ip_, name);
	ocssds_.push_back(unit);
	count_++;
	return 0;
}

size_t ocssd_manager::alloc_ocssd_resource(virtual_ocssd *vssd, ocssd_alloc_request *request)
{
	size_t channels = 0;

	MutexLock lock(&mutex_);

	for (auto unit : ocssds_) {
		size_t ret = unit->alloc_channels(vssd, request);

		channels += ret;

		request->dec_channels(ret);

		if (request->get_channels() == 0)
			break;
	}

	vssd->set_id(vssd_id_);
	vssd_id_++;
	return channels;
}

const std::vector<ocssd_unit *> & ocssd_manager::get_units()
{
	return ocssds_;
}
