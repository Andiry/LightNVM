#ifndef OCSSD_CONN_H
#define OCSSD_CONN_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <exception>

class ocssd_conn {
public:
	ocssd_conn(int connfd, const struct sockaddr_in &client);
	~ocssd_conn(){printf("%s\n", __func__);}

	void process() {printf("%s\n", __func__);}

private:
	ocssd_conn(const ocssd_conn &);
	ocssd_conn & operator=(const ocssd_conn &);

	int connfd_;
	std::string ipaddr_;
};

ocssd_conn::ocssd_conn(int connfd, const struct sockaddr_in &client)
	: connfd_(connfd), ipaddr_(inet_ntoa(client.sin_addr))
	{
		std::cout << "New connection: conn " << connfd_
			<< ", IP addr " << ipaddr_ << std::endl;
	}


#endif
