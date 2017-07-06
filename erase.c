#include<stdio.h>
#include<stdint.h>
#include<liblightnvm.h>

int main(int argc, char **argv) {
	struct nvm_dev *dev = nvm_dev_open("/dev/nvme0n1");
	struct nvm_addr addr1, addr2;
	uint16_t flags = 0;
	struct nvm_ret ret;
	struct nvm_addr addrs[2];

	if (!dev) {
		perror("nvm_dev_open");
		return 1;
	}

	addr1.g.ch = 0;
	addr1.g.lun = 0;
	addr1.g.pl = 0;
	addr1.g.sec = 0;
	addr1.g.pg = 0;
	addr1.g.blk = 10;

	addr2.g.ch = 0;
	addr2.g.lun = 0;
	addr2.g.pl = 1;
	addr2.g.sec = 0;
	addr2.g.pg = 0;
	addr2.g.blk = 10;

	addrs[0] = addr1;
	addrs[1] = addr2;

	printf("erase return %lu\n", nvm_addr_erase(dev, addrs,
			2, flags, &ret));

	printf("ret status %llu, result %u\n", ret.status, ret.result);

	nvm_dev_close(dev);
	return 0;
}
