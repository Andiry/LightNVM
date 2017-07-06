#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include<liblightnvm.h>

int main(int argc, char **argv) {
	struct nvm_dev *dev = nvm_dev_open("/dev/nvme0n1");
	const struct nvm_geo *geo;
	uint16_t flags = 0;
	struct nvm_ret ret;
	struct nvm_addr addrs[8];
	int i = 0;
	void *buf;
	void *meta;
	ssize_t res;

	if (!dev) {
		perror("nvm_dev_open");
		return 1;
	}

	for (i = 0; i < 8; i++) {
		addrs[i].g.ch = 0;
		addrs[i].g.lun = 0;
		addrs[i].g.pl = i / 4;
		addrs[i].g.sec = i % 4;
		addrs[i].g.pg = 0;
		addrs[i].g.blk = 10;
	}

	geo = nvm_dev_get_geo(dev);

	buf = nvm_buf_alloc(geo, 4096 * 8);
	if (!buf) {
		printf("nvm_buf_alloc failed\n");
		goto out;
	}

	for (i = 0; i < 4096 * 8; i++)
		((char *)buf)[i] = (i % 26 + 65);

	meta = nvm_buf_alloc(geo, geo->meta_nbytes * 8);
	if (!meta) {
		printf("nvm_buf_alloc failed\n");
		free(buf);
		goto out;
	}

	res = nvm_addr_write(dev, addrs, 8, buf, meta, flags, &ret);

	printf("write return %lu\n", res);

	printf("ret status %llu, result %u\n", ret.status, ret.result);

	free(buf);
	free(meta);
out:
	nvm_dev_close(dev);
	return 0;
}
