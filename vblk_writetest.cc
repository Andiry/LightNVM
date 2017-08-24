#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include<time.h>
#include<string.h>
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

static int test_vblk(struct nvm_dev *dev, const std::vector<int> & channels, int offset)
{
	const struct nvm_geo *geo;
	struct nvm_addr addr;
	std::vector<struct nvm_addr> units;
	struct nvm_vblk *blk;
	void *buf;
	ssize_t res;
	struct timespec begin, finish;
	unsigned long time1;
	int end_size = 0;
	int start_size = 0;
	int i;

	geo = nvm_dev_get_geo(dev);

	end_size = geo->nplanes * geo->npages * geo->nsectors * geo->sector_nbytes;
	start_size = geo->nplanes * geo->nsectors * geo->sector_nbytes;

	for (int channel : channels) {
		addr.ppa = 0;
		addr.g.ch = channel;

		units.push_back(addr);
	}

	blk = nvm_vblk_alloc(dev, units.data(), units.size());

	clock_gettime(CLOCK_MONOTONIC, &begin);
	nvm_vblk_erase(blk);
	clock_gettime(CLOCK_MONOTONIC, &finish);

	printf("vblk channels %lu, size %lu\n", channels.size(), nvm_vblk_get_nbytes(blk));
	time1 = (finish.tv_sec * 1e9 + finish.tv_nsec) - (begin.tv_sec * 1e9 + begin.tv_nsec);
	printf("Erase %lu ns\n", time1);

	buf = nvm_buf_alloc(geo, end_size);
	if (!buf) {
		printf("nvm_buf_alloc failed\n");
		goto out;
	}

	memset(buf, 0, end_size);

	while (start_size <= end_size) {
		nvm_vblk_erase(blk);
		nvm_vblk_set_pos_write(blk, 0);

		clock_gettime(CLOCK_MONOTONIC, &begin);
		for (i = 0; i < end_size / start_size; i++) {
			res = nvm_vblk_write(blk, buf, start_size);
			if (res < 0)
				printf("Channel %lu write return %lu, %d\n", channels.size(), res, errno);
		}
		clock_gettime(CLOCK_MONOTONIC, &finish);

		time1 = (finish.tv_sec * 1e9 + finish.tv_nsec) - (begin.tv_sec * 1e9 + begin.tv_nsec);
		printf("size %d, Write %lu ns, bandwidth %.2f MB/s\n", start_size, time1, (16 * 1e9 / time1));
		start_size *= 2;
	}

	free(buf);
out:
	nvm_vblk_free(blk);

	return 0;
}

int main(int argc, char **argv) {
	struct nvm_dev *dev;
	const struct nvm_geo *geo;
	std::vector<int> channels;
	std::string dev1("/dev/nvme0n1");
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

	if (dev1.compare(argv[1]) == 0) {
		/* Remove dead channels */
		while (channel < geo->nchannels) {
			if ((channel / 2) % 2 == 1) {
				channel++;
				continue;
			}
			channels.push_back(channel);
			test_vblk(dev, channels, d);
			channel++;
		}
	} else {
		for (channel = 0; channel < geo->nchannels; channel++) {
			channels.push_back(channel);
			test_vblk(dev, channels, d);
		}
	}

	nvm_dev_close(dev);
	return 0;
}


