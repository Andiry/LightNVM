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

/*
 * Request serialize format:
 * REQUEST_MAGIC	4 bytes
 * NUM_CHANNELS		4 bytes
 * NUM_BLOCKS		4 bytes
 * SHARED		4 bytes
 * NUMA_ID		4 bytes
 */
class ocssd_alloc_request {
public:

	ocssd_alloc_request(size_t num_channels, size_t num_blocks, int shared, int numa_id)
		: num_channels_(num_channels),
		num_blocks_(num_blocks),
		shared_(shared),
		numa_id_(numa_id) {}

	ocssd_alloc_request(size_t num_channels, int numa_id)
		: num_channels_(num_channels),
		num_blocks_(0),
		shared_(0),
		numa_id_(numa_id) {}

	ocssd_alloc_request(const char *buffer) {
		uint32_t magic = deserialize_data(buffer);
		if (magic != REQUEST_MAGIC) {
			printf("Incorrect MAGIC: %x\n", magic);
			throw std::runtime_error("Error: init request failed\n");
		}

		num_channels_	= deserialize_data(buffer);
		num_blocks_	= deserialize_data(buffer);
		shared_		= deserialize_data(buffer);
		numa_id_	= deserialize_data(buffer);

		printf("Request %u channels, %u blocks, shared %u, NUMA %u\n",
			num_channels_, num_blocks_, shared_, numa_id_);
	}

	size_t serialize(char *buffer) {
		char *start = buffer;
		serialize_data(buffer, REQUEST_MAGIC);
		serialize_data(buffer, num_channels_);
		serialize_data(buffer, num_blocks_);
		serialize_data(buffer, shared_);
		serialize_data(buffer, numa_id_);
		return buffer - start;
	}

	uint32_t get_channels() {return num_channels_;}
	uint32_t get_blocks() {return num_blocks_;}
	void dec_channels(size_t channels) {num_channels_ -= channels;}
	uint32_t get_shared() {return shared_;}
	uint32_t get_numa_id() {return numa_id_;}

private:

	uint32_t num_channels_;
	uint32_t num_blocks_;
	uint32_t shared_;
	uint32_t numa_id_;
};

/*
 * Virtual OCSSD serialization format:
 * SERIALIZE_MAGIC		4 bytes
 * NUM_UNITS			4 bytes
 *	DEV_NAME_LEN		4 bytes
 *	DEV_NAME		4 bytes * N
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

	size_t serialize(char *&buffer);

private:
	uint32_t lun_id_;
	uint32_t block_start_;
	uint32_t num_blocks_;
};

size_t virtual_ocssd_lun::serialize(char *&buffer) {
	char *start = buffer;
	char *&p = buffer;

	serialize_data(p, lun_id_);
	serialize_data(p, block_start_);
	serialize_data(p, num_blocks_);

	return p - start;
}

virtual_ocssd_lun::virtual_ocssd_lun(const char *&buffer) {
	const char *&p = buffer;

	lun_id_ = deserialize_data(p);
	block_start_ = deserialize_data(p);
	num_blocks_ = deserialize_data(p);

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

	void generate_units(std::vector<int> &units);
	size_t serialize(char *&buffer);
	size_t deserialize(const char *&buffer);

	void add(virtual_ocssd_lun * lun) {
		luns_.push_back(lun);
	}

	const uint32_t get_channel_id() const {return channel_id_;}
	bool is_shared() const {return shared_ > 0;}
	const uint32_t get_total_blocks() const {return total_blocks_;}
	const uint32_t get_num_luns() const {return num_luns_;}
	const std::vector<virtual_ocssd_lun *>& get_luns() const {return luns_;}

private:
	uint32_t channel_id_;
	uint32_t shared_;
	uint32_t total_blocks_;
	uint32_t num_luns_;
	std::vector<virtual_ocssd_lun *> luns_;
};

void virtual_ocssd_channel::generate_units(std::vector<int> &units)
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

size_t virtual_ocssd_channel::serialize(char *&buffer) {
	char *start = buffer;
	char *&p = buffer;

	serialize_data(p, channel_id_);
	serialize_data(p, shared_);
	serialize_data(p, total_blocks_);
	serialize_data(p, num_luns_);

	if (shared_ == 1) {
		for (virtual_ocssd_lun * lun : luns_)
			lun->serialize(p);
	}

	return p - start;
}

size_t virtual_ocssd_channel::deserialize(const char *&buffer) {
	const char *start = buffer;
	const char *&p = buffer;

	channel_id_ = deserialize_data(p);
	shared_ = deserialize_data(p);
	total_blocks_ = deserialize_data(p);
	num_luns_ = deserialize_data(p);

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

class virtual_ocssd_unit {
public:

	virtual_ocssd_unit(std::string dev_name = "")
		:dev_name_(dev_name) {}

	~virtual_ocssd_unit() {
		for (auto channel : channels_)
			delete channel;
	}

	void generate_units(std::vector<int> &units) const;
	size_t serialize(char *&buffer);
	size_t deserialize(const char *&buffer);
	void add(virtual_ocssd_channel *channel) {channels_.push_back(channel);}

	const std::string& get_dev_name() const {return dev_name_;}
	const std::vector<virtual_ocssd_channel *>& get_channels() const {return channels_;}

private:
	std::string dev_name_;
	std::vector<virtual_ocssd_channel *> channels_;
};

void virtual_ocssd_unit::generate_units(std::vector<int> &units) const
{
	for (virtual_ocssd_channel *channel : channels_)
		channel->generate_units(units);
}

size_t virtual_ocssd_unit::serialize(char *&buffer) {
	char *start = buffer;
	char *&p = buffer;
	size_t len = dev_name_.length() + 1;

	serialize_data(p, len);
	memcpy(p, dev_name_.c_str(), len);
	len = (len + 3) / 4 * 4;
	p += len;

	serialize_data(p, channels_.size());

	for (virtual_ocssd_channel *channel : channels_)
		channel->serialize(p);

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

	uint32_t num_channel = deserialize_data(p);
	std::cout<< "Device " << dev_name_ << ": "
		 << num_channel << " channels" << std::endl;

	for (uint32_t i = 0; i < num_channel; i++) {
		virtual_ocssd_channel *channel = new virtual_ocssd_channel();
		channel->deserialize(p);
		channels_.push_back(channel);
	}

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

	size_t get_num_units() const {return units_.size();}
	const virtual_ocssd_unit *get_unit(int i) const {return units_[i];}

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

	size_t get_num_luns() {
		return num_luns_;
	}

	size_t alloc_blocks(std::vector<std::pair<size_t, std::pair<size_t, size_t>>> & alloc_units,
					size_t request_blocks) {
		if (num_used_blocks_ == num_total_blocks_ || request_blocks == 0)
			return 0;

		size_t allocated = 0;

		for (ocssd_lun * lun : luns_) {
			if (lun->get_num_used_blocks() >= num_blocks_)
				continue;

			std::pair<size_t, size_t> block_units;
			size_t ret = lun->alloc_blocks(block_units, request_blocks);

			alloc_units.push_back(std::pair<size_t, std::pair<size_t, size_t>>
						(lun->get_lun_id(), block_units));

			request_blocks -= ret;
			allocated += ret;
			if (request_blocks == 0)
				break;
		}

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

class ocssd_unit {
public:

	ocssd_unit(std::string name) :name_(name) {
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

	size_t alloc_channels(virtual_ocssd *vssd, ocssd_alloc_request *request);

private:

	int initialize_dev();
	int channel_ok(size_t channel_id);
	int assign_to_shared(ocssd_channel *channel);

	size_t alloc_shared_channels(virtual_ocssd_unit *vunit,
		virtual_ocssd *vssd, ocssd_alloc_request *request);
	size_t alloc_exclusive_channels(virtual_ocssd_unit *vunit,
		virtual_ocssd *vssd, ocssd_alloc_request *request);

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
	printf("geo: %lu channels, %lu LUNs per channel, %lu blocks per LUN\n",
		geo_->nchannels, geo_->nluns, geo_->nblocks);

	for (size_t channel_id = 0; channel_id < geo_->nchannels; channel_id++) {
		if (channel_ok(channel_id)) {
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
				goto out;
		}
	}

out:
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

	virtual_ocssd_unit *vunit = new virtual_ocssd_unit(name_);

	mutex_.lock();

	if (request->get_shared() == 1)
		channels = alloc_shared_channels(vunit, vssd, request);
	else
		channels = alloc_exclusive_channels(vunit, vssd, request);

	mutex_.unlock();

	if (channels == 0)
		delete vunit;
	else
		vssd->add(vunit);

	return channels;
}

class ocssd_manager {
public:

	int add_ocssd(std::string name);

	~ocssd_manager() {
		for (auto pair : ocssds_)
			delete pair.second;
	}

	size_t alloc_ocssd_resource(virtual_ocssd *vssd, ocssd_alloc_request *request);

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

size_t ocssd_manager::alloc_ocssd_resource(virtual_ocssd *vssd, ocssd_alloc_request *request)
{
	size_t channels = 0;

	mutex_.lock();

	for (auto pair : ocssds_) {
		ocssd_unit *unit = pair.second;
		size_t ret = unit->alloc_channels(vssd, request);

		channels += ret;

		request->dec_channels(ret);

		if (request->get_channels() == 0)
			break;
	}

	mutex_.unlock();

	return channels;
}
