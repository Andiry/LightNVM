#include <liblightnvm.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>

static inline void serialize_data(char *&buffer, uint32_t data) {
	char *&p = buffer;
	*(uint32_t *)p = data;
	p += sizeof(uint32_t);
}

static inline uint32_t deserialize_data(const char *&buffer) {
	uint32_t data;
	const char *&p = buffer;
	data = *(uint32_t *)p;
	p += sizeof(uint32_t);
	return data;
}

#define REQUEST_MAGIC 0x6501

class ocssd_alloc_request {
public:

	ocssd_alloc_request(int num_channels, int shared = 0, int numa_id = 0)
		: num_channels_(num_channels),
		shared_(shared),
		numa_id_(numa_id) {}

	ocssd_alloc_request(const char *buffer) {
		uint32_t magic = deserialize_data(buffer);
		if (magic != REQUEST_MAGIC) {
			printf("Incorrect MAGIC: %x\n", magic);
			throw std::runtime_error("Error: init request failed\n");
		}

		num_channels_	= deserialize_data(buffer);
		shared_		= deserialize_data(buffer);
		numa_id_	= deserialize_data(buffer);

		printf("Request %u channels, shared %u, NUMA %u\n",
			num_channels_, shared_, numa_id_);
	}

	size_t serialize(char *buffer) {
		char *start = buffer;
		serialize_data(buffer, REQUEST_MAGIC);
		serialize_data(buffer, num_channels_);
		serialize_data(buffer, shared_);
		serialize_data(buffer, numa_id_);
		return buffer - start;
	}

private:

	uint32_t num_channels_;
	uint32_t shared_;
	uint32_t numa_id_;
};

class virtual_ocssd_unit {
public:

	virtual_ocssd_unit(std::string dev_name = "")
		:dev_name_(dev_name) {}

	size_t serialize(char *&buffer);
	size_t deserialize(const char *&buffer);
	void add(uint32_t channel) {channels_.push_back(channel);}

private:
	std::string dev_name_;
	std::vector<uint32_t> channels_;
};

/*
 * Serialize format:
 * SERIALIZE_MAGIC		4 bytes
 * NUM_UNITS			4 bytes
 *	DEV_NAME_LEN		4 bytes
 *	DEV_NAME		4 bytes * N
 *	NUM_CHANNELS		4 bytes
 *		CHANNEL_ID	4 bytes
 *		NUM_LUNS	4 bytes
 *		LUN_ID		4 bytes
 *		LUN_ID		4 bytes
 *		...
 *
 */
size_t virtual_ocssd_unit::serialize(char *&buffer) {
	char *start = buffer;
	char *&p = buffer;
	size_t len = dev_name_.length() + 1;

	serialize_data(p, len);
	memcpy(p, dev_name_.c_str(), len);
	len = (len + 3) / 4 * 4;
	p += len;

	serialize_data(p, channels_.size());

	for (int channel : channels_)
		serialize_data(p, channel);

	return p - start;
}

size_t virtual_ocssd_unit::deserialize(const char *&buffer) {
	const char *start = buffer;
	const char *&p = buffer;
	uint32_t name_len = deserialize_data(p);
	const char *temp = p;

	dev_name_ = temp;
	name_len = (name_len + 3) / 4 * 4;
	p += name_len;

	uint32_t num_lun = deserialize_data(p);
	std::cout<< "Device " << dev_name_ << ": " << num_lun << " LUNs" << std::endl;

	for (uint32_t i = 0; i < num_lun; i++)
		channels_.push_back(deserialize_data(p));

	return p - start;
}

class virtual_ocssd {
public:
	virtual_ocssd() {}
	~virtual_ocssd() {
		for (auto p : units_)
			delete p;
	}

	size_t serialize(char *buffer);
	size_t deserialize(const char *buffer);
	void add(virtual_ocssd_unit *unit) {units_.push_back(unit);}

private:

	std::vector<virtual_ocssd_unit *> units_;
};

#define SERIALIZE_MAGIC 0x6502

size_t virtual_ocssd::serialize(char *buffer) {
	char *p = buffer;

	serialize_data(p, SERIALIZE_MAGIC);
	serialize_data(p, units_.size());

	for (virtual_ocssd_unit *unit : units_)
		unit->serialize(p);

	return p - buffer;
}

size_t virtual_ocssd::deserialize(const char *buffer) {
	const char *p = buffer;
	uint32_t magic;
	uint32_t num_unit = 0;

	magic = deserialize_data(p);
	if (magic != SERIALIZE_MAGIC) {
		printf("Incorrect MAGIC: %x\n", magic);
		return 0;
	}

	num_unit = deserialize_data(p);
	printf("%u Devices\n", num_unit);

	for (uint32_t i = 0; i < num_unit; i++) {
		virtual_ocssd_unit *unit = new virtual_ocssd_unit();
		unit->deserialize(p);
		units_.push_back(unit);
	}

	return p - buffer;
}

class ocssd_channel {
public:

	ocssd_channel(size_t channel_id, size_t num_luns, size_t num_blocks)
		:channel_id_(channel_id),
		num_luns_(num_luns),
		num_blocks_(num_blocks),
		used_count_(0)
	{
		lun_used_ = new int[num_luns_];
	}

	~ocssd_channel() {
		delete[] lun_used_;
	}

private:

	size_t channel_id_;
	size_t num_luns_;
	size_t num_blocks_;
	size_t used_count_;
	int *lun_used_;
};

class ocssd_unit {
public:

	ocssd_unit(std::string name) :name_(name) {
		dev_ = nvm_dev_open(name.c_str());
		if (!dev_)
			throw std::runtime_error("Error: open dev failed\n");

		initialize_dev();
	}

	~ocssd_unit() {
		for (unsigned int i = 0; i < shared_channels_.size(); i++)
			delete shared_channels_[i];

		for (unsigned int i = 0; i < exclusive_channels_.size(); i++)
			delete exclusive_channels_[i];

		nvm_dev_close(dev_);
	}

	std::string get_name() {
		return name_;
	}

private:

	int initialize_dev();
	int channel_ok(size_t channel_id);
	int assign_to_shared(ocssd_channel *channel);

	std::string name_;
	struct nvm_dev *dev_;
	const struct nvm_geo *geo_;
	std::mutex mutex_;
	int channel_count_ = 0;
	std::vector<ocssd_channel *> shared_channels_;
	std::vector<ocssd_channel *> exclusive_channels_;
};

int ocssd_unit::channel_ok(size_t channel_id)
{
	std::vector<struct nvm_addr> addrs;
	struct nvm_addr addr;
	struct nvm_vblk *blk;
	int size = 4096 * 8;
	void *buf;
	ssize_t res;
	int ret;

	addr.ppa = 0;
	addr.g.ch = channel_id;
	addr.g.lun = 0;

	addrs.push_back(addr);

	blk = nvm_vblk_alloc(dev_, addrs.data(), addrs.size());

	buf = nvm_buf_alloc(geo_, size);
	if (!buf) {
		printf("nvm_buf_alloc failed\n");
		return -ENOMEM;
	}

	res = nvm_vblk_read(blk, buf, size);

	ret = res == size ? 1 : 0;

	nvm_vblk_free(blk);
	free(buf);
	return ret;
}

int ocssd_unit::assign_to_shared(ocssd_channel *channel)
{
	if (shared_channels_.size() < 4)
		return 1;

	return 0;
}

int ocssd_unit::initialize_dev()
{
	geo_ = nvm_dev_get_geo(dev_);
	printf("geo: %lu channels, %lu LUNs, %lu blocks\n",
		geo_->nchannels, geo_->nluns, geo_->nblocks);

	for (size_t channel_id = 0; channel_id < geo_->nchannels; channel_id++) {
		if (channel_ok(channel_id)) {
			ocssd_channel *channel = new ocssd_channel(channel_id,
							geo_->nluns,
							geo_->nblocks);

			if (assign_to_shared(channel))
				shared_channels_.push_back(channel);
			else
				exclusive_channels_.push_back(channel);

			channel_count_++;
		}
	}

	std::cout << "Get " << channel_count_ << " channels for " << name_ << std::endl;
	std::cout << "Share " << shared_channels_.size() << " channels, "
		  << "exclusive " << exclusive_channels_.size() << " channels" << std::endl;

	return 0;
}

class ocssd_manager {
public:

	int add_ocssd(std::string name);

	~ocssd_manager() {
		for (unsigned int i = 0; i < ocssds_.size(); i++)
			delete ocssds_[i];
	}

private:

	std::mutex mutex_;
	std::unordered_map<int, ocssd_unit *> ocssds_;
	int count_ = 0;
};

int ocssd_manager::add_ocssd(std::string name)
{
	ocssd_unit *unit = new ocssd_unit(name);
	mutex_.lock();
	ocssds_[count_] = unit;
	count_++;
	mutex_.unlock();
	return 0;
}
