#include <liblightnvm.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>

#define OCSSD_MAGIC 0xDEADBEEF

class ocssd_alloc_request {
public:
	uint64_t magic_;
	int num_channels_;
	int shared_;
	int numa_id_;

	ocssd_alloc_request(int num_channels, int shared = 0, int numa_id = 0)
		:magic_(OCSSD_MAGIC),
		num_channels_(num_channels),
		shared_(shared),
		numa_id_(numa_id) {}
};

class virtual_ocssd_unit {
public:
	std::string dev_name_;
	std::vector<int> channels_;

	virtual_ocssd_unit(std::string dev_name)
		:dev_name_(dev_name) {}
};

class virtual_ocssd {
public:
	std::vector<virtual_ocssd_unit> units_;
	int count;

	virtual_ocssd() :count(0) {}
};

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
