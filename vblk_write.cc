#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include<time.h>
#include<sys/time.h>
#include<liblightnvm.h>
#include<iostream>
#include<vector>
#include<deque>

using namespace std;

enum BlkState {
	kFree		= 0x1,
	kOpen		= 0x2,
	kReserved	= 0x4,
	kBad		= 0x8,
};

int main(int argc, char **argv) {
	struct nvm_dev *dev = nvm_dev_open("/dev/nvme0n1");
	const struct nvm_geo *geo;
	struct nvm_addr addr;
	std::vector<struct nvm_addr> units;
	std::deque<std::pair<BlkState, struct nvm_vblk *>> blks;
	struct nvm_vblk *blk;
	int i = 0, d = 0;
	int channel = 0;
	void *buf;
	ssize_t res;
	struct timespec begin, finish;
	long long time1;

	if (argc < 3) {
		printf("usage: ./write $CHANNEL $OFFSET\n");
		return 1;
	}

	channel = atoi(argv[1]);
	d = atoi(argv[2]);

	if (!dev) {
		perror("nvm_dev_open");
		return 1;
	}

	geo = nvm_dev_get_geo(dev);

	addr.ppa = 0;
	addr.g.ch = channel % geo->nchannels;
	addr.g.lun = (channel / geo->nchannels) % geo->nluns;

	units.push_back(addr);

	for (size_t blk_idx = 0; blk_idx < geo->nblocks; blk_idx++) {
		std::vector<struct nvm_addr> addrs(units);

		for (auto &k : addrs)
			k.g.blk = blk_idx;

		blk = nvm_vblk_alloc(dev, addrs.data(), addrs.size());
		blks.push_back(std::make_pair(kFree, blk));
	}

	blk = blks[0].second;

	clock_gettime(CLOCK_MONOTONIC, &begin);
	nvm_vblk_erase(blk);
	clock_gettime(CLOCK_MONOTONIC, &finish);

	time1 = (finish.tv_sec * 1e9 + finish.tv_nsec) - (begin.tv_sec * 1e9 + begin.tv_nsec);
	printf("Erase %lld ns, average %lld ns\n", time1, time1 / 1);

	buf = nvm_buf_alloc(geo, 4096 * 8);
	if (!buf) {
		printf("nvm_buf_alloc failed\n");
		goto out;
	}

	for (i = 0; i < 4096 * 8; i++)
		((char *)buf)[i] = (i % 26 + 65 + d);

	clock_gettime(CLOCK_MONOTONIC, &begin);
	res = nvm_vblk_write(blk, buf, 4096 * 8);
	printf("write return %lu, %d\n", res, errno);
	res = nvm_vblk_pwrite(blk, buf, 4096 * 8, 4096 * 8);
	printf("write return %lu, %d\n", res, errno);
	clock_gettime(CLOCK_MONOTONIC, &finish);

	time1 = (finish.tv_sec * 1e9 + finish.tv_nsec) - (begin.tv_sec * 1e9 + begin.tv_nsec);
	printf("Write %lld ns, average %lld ns\n", time1, time1 / 1);

	printf("write return %lu, %d\n", res, errno);

	free(buf);
out:
	nvm_dev_close(dev);
	return 0;
}
