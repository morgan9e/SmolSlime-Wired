#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
static inline char *inet_ntoa_r(struct in_addr a, char *buf, int len)
{
	inet_ntop(AF_INET, &a, buf, (socklen_t)len);
	return buf;
}
