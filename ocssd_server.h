#include <liblightnvm.h>
#include <vector>

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
	struct nvm_dev *dev_;
	std::vector<int> channels_;

	virtual_ocssd_unit(struct nvm_dev *dev)
		:dev_(dev) {}
};

class virtual_ocssd {
public:
	std::vector<virtual_ocssd_unit *> units_;
	int count;

	virtual_ocssd() :count(0) {}
};

class ocssd_channel {
public:
	int channel_id_;
	int num_blocks_;
	int used_count_;
	int shared_;

	ocssd_channel(int channel_id, int num_blocks, int shared = 0)
		:channel_id_(channel_id),
		num_blocks_(num_blocks),
		used_count_(0),
		shared_(shared) {}
};

class ocssd_unit {
public:
	struct nvm_dev *dev_;
	std::vector<ocssd_channel *> channels_;

	ocssd_unit(struct nvm_dev *dev)
		:dev_(dev) {}
};

