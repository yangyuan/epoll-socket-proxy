// std
#include <stdio.h>
#include <map>

// network
#include <arpa/inet.h>
#include <sys/socket.h>

// common
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>
#include <fcntl.h>

#define PACKET_BUFFER_SIZE 8192
#define EPOLL_BUFFER_SIZE 256

typedef struct {
	int src_fd;
	int tar_fd;
	int length;
	int offset;
	char buffer[PACKET_BUFFER_SIZE];
} LINK;


class StreamSocketProxy {
public:
	StreamSocketProxy() {}
	~StreamSocketProxy() {}
	
	bool init(const char * src_host, unsigned int src_port, const char * tar_host, unsigned int tar_port);
	void serve();
private:
	struct sockaddr_in src_addr;
	struct sockaddr_in tar_addr;
	
	int sockfd;
	int epollfd;
	std::map <int, LINK*> links;
	
	void on_link  (int fd);
	void on_break (int fd);
	void on_data_in  (int recv_fd);
	void on_data_out (int send_fd);
	bool fetch_link (int fd, int * fdx, LINK ** link);
	
	static int do_tcp_listen (struct sockaddr_in * _addr);
	static int do_tcp_connect (struct sockaddr_in * _addr);
	static int do_tcp_accpet (int listenfd);
	static int do_tcp_send (LINK * link, int fd);
	static int do_tcp_recv (LINK * link, int fd);
};

bool StreamSocketProxy::init(const char * src_host, unsigned int src_port, const char * tar_host, unsigned int tar_port) {
	// sockaddr
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(src_port);
	if (src_host == NULL) {
		src_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	} else {
		if (inet_pton(AF_INET, src_host, &src_addr.sin_addr) <= 0) { return false; }
	}
	tar_addr.sin_family = AF_INET;
    tar_addr.sin_port = htons(tar_port);
    if (inet_pton(AF_INET, tar_host, &tar_addr.sin_addr) <= 0) { return false; }

	// listen
	sockfd = do_tcp_listen(&src_addr);
	if (sockfd<0) { return false; }
	
	// epoll
	epollfd = epoll_create(EPOLL_BUFFER_SIZE); // epoll_create(int size); size is no longer used
	
	// epoll <--> listen
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd  = sockfd;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev);

	return true;
}

void StreamSocketProxy::serve() {
	struct epoll_event events[EPOLL_BUFFER_SIZE];
	int count;
	int i;
	while(true) {
		count = epoll_wait(epollfd, events, EPOLL_BUFFER_SIZE, -1);
		if( count < 0) {
			if (errno == EINTR) continue;
		}
		for (i = 0; i < count; i ++) {
			if ( events[i].data.fd == sockfd ) {
				on_link(sockfd);
			} else {
				if (events[i].events & EPOLLOUT) {
					on_data_out(events[i].data.fd);
				}
				if (events[i].events & EPOLLIN) {
					on_data_in (events[i].data.fd);
				}
			}
		}
	}
}

void StreamSocketProxy::on_link (int fd) {

	LINK * link = new LINK();

	link->src_fd = do_tcp_accpet (fd);
	link->tar_fd = do_tcp_connect (&tar_addr);
	link->length = 0;
	link->offset = 0;
	
	int flags;
	flags = fcntl(link->src_fd, F_GETFL, 0);    
	fcntl(link->src_fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(link->tar_fd, F_GETFL, 0);    
	fcntl(link->tar_fd, F_SETFL, flags | O_NONBLOCK);
	
	links.insert(std::pair<int, LINK*>(link->src_fd, link));
	links.insert(std::pair<int, LINK*>(link->tar_fd, link));

	// event
	struct epoll_event ev;
	ev.data.fd = link->src_fd;
	ev.events = EPOLLIN;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, link->src_fd, &ev);
	ev.data.fd = link->tar_fd;
	ev.events = EPOLLIN;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, link->tar_fd, &ev);
	
	printf("OPEN: %d <--> %d\n", link->src_fd, link->tar_fd);
	return;
}




void StreamSocketProxy::on_break (int fd) {
	// link
	LINK * link;
	int fdx;
	if (!fetch_link(fd, &fdx, &link)) { return; }
	
	// socket
	close(fd); // maybe shutdown (fd, SHUT_RDWR);
	close(fdx);
	
	// links
	links.erase(fd);
	links.erase(fdx);
	delete link;
	
	// event
	struct epoll_event ev;
	ev.data.fd = fd;
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
	ev.data.fd = fdx;
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fdx, &ev);
	
	printf("CLOSE: %d <--> %d\n", fd, fdx);
	return;
}

bool StreamSocketProxy::fetch_link(int fd, int * fdx, LINK ** link) {
	if (links.find(fd) != links.end()) {
		*link = links.find(fd)->second;
		if (fd == (*link)->src_fd) *fdx = (*link)->tar_fd;
		else *fdx = (*link)->src_fd;
		return true;
	} else {
		printf("BROKEN: %d <-- ?\n", fd);
		return false;
	}
}



void StreamSocketProxy::on_data_in (int recv_fd) {
	// link
	LINK * link;
	int send_fd = 0;
	if (!fetch_link(recv_fd, &send_fd, &link)) { return; }

	if (link->length != 0) {
		// wait buffer to be empty
		return;
	}
	
	// recv
	int ret = do_tcp_recv (link, recv_fd);
	if (ret == 0) {
		return;
	} else if (ret < 0) {
		on_break(recv_fd);
	}
	
	// send
	ret = do_tcp_send (link, send_fd);
	if (ret == 0) {
		// start watch EPOLLOUT
		struct epoll_event ev;
		ev.data.fd = send_fd;
		ev.events = EPOLLIN|EPOLLOUT;
		epoll_ctl(epollfd, EPOLL_CTL_MOD, send_fd, &ev);
	} else {
		if (ret < 0) {
			// FIXME: error, should close
		}
	}
	
	return;
}

void StreamSocketProxy::on_data_out (int send_fd) {
	// link
	LINK * link;
	int recv_fd = 0;
	if (!fetch_link(send_fd, &recv_fd, &link)) { return; }

	// send
	int ret = do_tcp_send (link, send_fd);
	if (ret == 0) {
		// keep watch EPOLLOUT
	} else {
		struct epoll_event ev;
		ev.data.fd = send_fd;
		ev.events = EPOLLIN;
		epoll_ctl(epollfd, EPOLL_CTL_MOD, send_fd, &ev);
		
		if (ret < 0) {
			// FIXME: error, should close
		}
	}
	
	return;
}

int StreamSocketProxy::do_tcp_listen (struct sockaddr_in * _addr) {
    const int flag = 1;
	int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) { return -1; }
    if (bind(listenfd,(const struct sockaddr*)_addr, sizeof(struct sockaddr_in)) < 0) { return -1; }
    if (listen(listenfd, SOMAXCONN) < 0) { return -1; }
    return listenfd;
}

int StreamSocketProxy::do_tcp_connect (struct sockaddr_in * _addr) {
    const int flag = 1;
    int connectfd = socket(AF_INET, SOCK_STREAM, 0);
    if(setsockopt(connectfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) { return -1; }
    if(connect(connectfd,(const struct sockaddr*)_addr, sizeof(struct sockaddr_in)) < 0) { return -1; }
    return connectfd;
}

int StreamSocketProxy::do_tcp_accpet (int listenfd) {
    struct sockaddr_in _addr;
	int socklen = sizeof(sockaddr_in);
	int acceptfd = accept(listenfd, (struct sockaddr *) & _addr, (socklen_t*) & socklen);
    return acceptfd;
}

// 1: success; 0: not finished; -1: closed (-? other error)
int StreamSocketProxy::do_tcp_send (LINK * link, int fd) {
	int ret;
	while(link->length > 0) {
		ret = send(fd, link->buffer + link->offset, link->length, 0);
		if (ret < 0) {
			if (errno == EAGAIN) {
				return 0;
			} else {
				return -1;
			}
		} else {
			link->length -= ret;
			link->offset += ret;
		}
	}
	return 1;
}

// 1: success; 0: not finished; -1: closed (-? other error)
int StreamSocketProxy::do_tcp_recv (LINK * link, int fd) {
	link->offset = 0;
	link->length = 0;
	
	int ret = recv(fd, link->buffer, PACKET_BUFFER_SIZE, 0);
	if (ret < 0) {
		if (errno == EAGAIN) {
			return 0;
		} else {
			return -1;
		}
	} else if (ret == 0) {
		return -1;
	}
	
	link->length = ret;
	return 1;
}

int main (int argc, char *argv[]) {
	StreamSocketProxy * sp = new StreamSocketProxy();
	bool ret = sp->init(NULL, 8080, "127.0.0.1", 80);
	sp->serve();
}
