#include <liblightnvm.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>

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
	int channel_id_;
	int num_blocks_;
	int used_count_;

	ocssd_channel(int channel_id, int num_blocks)
		:channel_id_(channel_id),
		num_blocks_(num_blocks),
		used_count_(0) {}
};

class ocssd_unit {
public:

	ocssd_unit(std::string name) :name_(name) {
		dev_ = nvm_dev_open(name.c_str());
		if (!dev_)
			throw std::runtime_error("Error: open dev failed\n");
	}

	~ocssd_unit() {
		for (unsigned int i = 0; i < shared_channels_.size(); i++)
			delete shared_channels_[i];

		for (unsigned int i = 0; i < exclusive_channels_.size(); i++)
			delete exclusive_channels_[i];

		nvm_dev_close(dev_);
	}

private:
	std::string name_;
	struct nvm_dev *dev_;
	std::vector<ocssd_channel *> shared_channels_;
	std::vector<ocssd_channel *> exclusive_channels_;
};

class ocssd_manager {
public:
	std::unordered_map<int, ocssd_unit *> ocssds_;

	~ocssd_manager() {
		for (unsigned int i = 0; i < ocssds_.size(); i++)
			delete ocssds_[i];
	}
};
