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

static int test_vblk(struct nvm_dev *dev, size_t channel, int offset)
{
	const struct nvm_geo *geo;
	struct nvm_addr addr;
	std::vector<struct nvm_addr> units;
	std::deque<std::pair<BlkState, struct nvm_vblk *>> blks;
	struct nvm_vblk *blk;
	void *buf;
	ssize_t res;
	struct timespec begin, finish;
	long long time1;
	int i;

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
		((char *)buf)[i] = (i % 26 + 65 + offset);

	clock_gettime(CLOCK_MONOTONIC, &begin);
	res = nvm_vblk_write(blk, buf, 4096 * 8);
	printf("Channel %lu write return %lu, %d\n", channel, res, errno);
	res = nvm_vblk_pwrite(blk, buf, 4096 * 8, 4096 * 8);
	printf("Channel %lu write return %lu, %d\n", channel, res, errno);
	clock_gettime(CLOCK_MONOTONIC, &finish);

	time1 = (finish.tv_sec * 1e9 + finish.tv_nsec) - (begin.tv_sec * 1e9 + begin.tv_nsec);
	printf("Write %lld ns, average %lld ns\n", time1, time1 / 1);

	printf("write return %lu, %d\n", res, errno);

	free(buf);
out:
	for (auto pair : blks)
		nvm_vblk_free(pair.second);

	return 0;
}

int main(int argc, char **argv) {
	struct nvm_dev *dev;
	const struct nvm_geo *geo;
	int d = 0;
	size_t channel = 0;

	if (argc < 2) {
		printf("usage: ./vblk_write $DEVICE\n");
		return 1;
	}

	dev = nvm_dev_open(argv[1]);

	if (!dev) {
		perror("nvm_dev_open");
		return 1;
	}

	geo = nvm_dev_get_geo(dev);

	for (channel = 0; channel < geo->nchannels; channel++)
		test_vblk(dev, channel, d);

	nvm_dev_close(dev);
	return 0;
}


