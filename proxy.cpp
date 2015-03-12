// std
#include <stdio.h>
#include <map>
#include <vector>
#include <iostream>
#include <sstream>

// network
#include <arpa/inet.h>
#include <sys/socket.h>

// system
#include <signal.h>
#include <pthread.h>
#include <sys/epoll.h>

// common
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>


#define PACKET_BUFFER_SIZE 8192
#define EPOLL_BUFFER_SIZE 256

typedef struct {
	int src_fd;
	int tar_fd;
	int length;
	int offset;
	char buffer[PACKET_BUFFER_SIZE];
} LINK;

class SocketProxyManager;

class StreamSocketProxy {
public:
	StreamSocketProxy() {}
	~StreamSocketProxy() {}
	
	bool startup(const char * src_host, unsigned int src_port, const char * tar_host, unsigned int tar_port);
	void serve();
	void shutdown();
private:
	struct sockaddr_in src_addr;
	struct sockaddr_in tar_addr;
	
	pthread_t thread;
	friend class SocketProxyManager;
	
	int sockfd;
	int epollfd;
	int pipefd[2];
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

bool StreamSocketProxy::startup(const char * src_host, unsigned int src_port, const char * tar_host, unsigned int tar_port) {
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
	
	// pipe
	if (pipe(pipefd)<0) { return false; }
	
	// epoll
	epollfd = epoll_create(EPOLL_BUFFER_SIZE); // epoll_create(int size); size is no longer used
	
	// epoll <--> listen, pipe
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd  = sockfd;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev);
	
	ev.data.fd  = pipefd[0];
	epoll_ctl(epollfd, EPOLL_CTL_ADD, pipefd[0], &ev);

	return true;
}

void StreamSocketProxy::serve() {
	struct epoll_event events[EPOLL_BUFFER_SIZE];
	int count;
	int i;
	
	while(true) {
		count = epoll_wait(epollfd, events, EPOLL_BUFFER_SIZE, -1);
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			} else {
				printf("epoll error\n");
				return;
			}
		}
		for (i = 0; i < count; i ++) {
			// 
			if ( events[i].data.fd == pipefd[0] ) {
				return;
			} else if ( events[i].data.fd == sockfd ) {
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

void StreamSocketProxy::shutdown() {
	close(sockfd);
	close(epollfd);
	close(pipefd[0]);
	close(pipefd[1]);
	
	for (std::map<int, LINK*>::iterator it=links.begin(); it!= links.end(); it++) {
		LINK * link = it->second;
		close(link->src_fd);
		close(link->tar_fd);
		delete link;
	}
	links.empty();
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



class SocketProxyManager {
public:
	void handle();
	
	StreamSocketProxy * add(const char * src_host, unsigned int src_port, const char * tar_host, unsigned int tar_port);
	void del(StreamSocketProxy *);
private:
	static void * proc (void * arg);
	static void onsignal(int sig);
	static int signo;
	
	std::vector<StreamSocketProxy *> proxies;
	
	void clear(bool force);
	
	void handle_command();
};

void * SocketProxyManager::proc (void * arg)
{
    StreamSocketProxy * sp = (StreamSocketProxy *) arg;
	sp->serve();
	sp->shutdown();
	return NULL;
}

StreamSocketProxy * SocketProxyManager::add (const char * src_host, unsigned int src_port, const char * tar_host, unsigned int tar_port) {
	StreamSocketProxy * sp = new StreamSocketProxy();
	bool ret = sp->startup(src_host, src_port, tar_host, tar_port);
	pthread_create(&(sp->thread), NULL, &proc, sp); // remember to pthread_join
	proxies.push_back(sp);
}

void SocketProxyManager::del (StreamSocketProxy * sp)
{
	return;
}

void SocketProxyManager::onsignal(int sig) {
	signo = sig;
	printf("\nSIGNAL: %d\n", signo);
}

int SocketProxyManager::signo = 0;


void SocketProxyManager::clear(bool force) {
	std::vector<StreamSocketProxy *>::iterator it = proxies.begin();
	
	while (it!= proxies.end()) {
		StreamSocketProxy * sp = (*it);
		int ret = 0;
		if (force) {
			pthread_cancel(sp->thread);
			pthread_join(sp->thread, NULL);
			printf("killed a thread\n");
			delete sp;
			it = proxies.erase(it);
		} else {
			ret = pthread_kill(sp->thread, 0);
			if (ret != 0) {
				// thread already died
				pthread_join(sp->thread, NULL);
				printf("died a thread\n");
				delete sp;
				it = proxies.erase(it);
			} else {
				it++;
				// printf("a thread\n");
			}
		}
	}
}



std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

void SocketProxyManager::handle_command()  
{
	std::string command;
	struct timeval tv;
	fd_set fds;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);
	if (FD_ISSET(0, &fds)) {
		std::getline(std::cin, command);
	} else {
		return;
	}
	
	std::vector<std::string> cmdx = split(command, ' ');
	if (cmdx.size() != 6) {
		std::cout << "invalid command: " << command << std::endl;
	}
	return;
}

void SocketProxyManager::handle() {
	signo = 0;
	signal (SIGINT, onsignal);
	signal (SIGTERM, onsignal);
	
	char buffer[128];
	
	int countx = 0;
	while (true) {
		
		// handle_command();

		countx++;
		if (countx == 3) {
			close(proxies[0]->pipefd[1]);
		}
		if (signo != 0) {
			printf("exit by signo %d\n", signo);
			break;
		}
		clear(false);
		usleep(125);
	}
	
	// 1 sec to clean up everything
	// close pipes
	for (std::vector<StreamSocketProxy *>::iterator it = proxies.begin(); it != proxies.end(); it ++) {
		close((*it)->pipefd[1]);
	}
	// clean closed threads
	int count = 8;
	while (count --) {
		clear(false);
		usleep(125);
	}
	// if there still some
	clear(true);
}

int main (int argc, char *argv[]) {
	SocketProxyManager * spm = new SocketProxyManager();
	StreamSocketProxy * sp = spm->add(NULL, 8080, "127.0.0.1", 80);
	spm->add(NULL, 8022, "127.0.0.1", 22);
	spm->handle();
}
