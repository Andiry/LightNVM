#include<stdio.h>
#include<stdint.h>
#include<liblightnvm.h>

int main(int argc, char **argv) {
	struct nvm_dev *dev = nvm_dev_open("/dev/nvme0n1");
	struct nvm_addr addr;

	if (!dev) {
		perror("nvm_dev_open");
		return 1;
	}

	addr.g.ch = 4;
	addr.g.lun = 1;
	addr.g.pl = 0;
	addr.g.sec = 3;
	addr.g.pg = 10;
	addr.g.blk = 200;

	printf("addr: 0x%llx\n", addr.ppa);
	printf("dev addr: 0x%llx\n", nvm_addr_gen2dev(dev, addr));

	nvm_dev_close(dev);
	return 0;
}
