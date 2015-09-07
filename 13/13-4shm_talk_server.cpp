#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<sys/epoll.h>
#include<signal.h>
#include<sys/wait.h>
#include<sys/mman.h>
#include<sys/stat.h>
#include<fcntl.h>

#define USER_LIMIT 5   //用户数量限制
#define BUFFER_SIZE 1024  //缓冲区大小
#define DF_LIMIT 65535   //最多可打开的文件描述符数
#define MAX_EVENT_NUMBER 1024  //最多可以注册的事件数
#define PROCESS_LIMIT 65536  //进程数量限制

//处理一个客户连接必要的数据
struct client_data 
{
	sockaddr_in address;  //客户端的socket地址
	int connfd;  //socket文件描述符
	pid_t pid;  //处理这个连接的子进程pid
	int pipefd[2];  //和父进程通信的管道
};

static const char* shm_name = "/my_shm";  
int sig_pipefd[2];
int epollfd; //标识内核事件表的fd
int listenfd; //监听socketfd
int shmfd;  //标识共享内存的fd
char* share_mem = NULL; 
client_data* users = NULL; 
int* sub_process = NULL; 
int user_count = 0; //用户数量
bool stop_child = false; //终止child标志

//设置文件描述符非阻塞
int setnonblocking(int fd)
{
	int old_option = fcntl(fd,F_GETFL);//获取原来的fd的状态标志
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd,F_SETFL,new_option);
	return old_option;
}

//向epollfd标识的内核表中添加文件描述符fd关心的事件
void addfd(int epollfd,int fd)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET;
	epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);//将fd上的event事件注册到epollfd标识的内核事件表中。
	setnonblocking(fd);//设置fd非阻塞
}

//信号处理函数
void sig_handler(int sig)
{
	int save_errno = errno;
	int msg = sig;
	send(sig_pipefd[1],(char*)&msg,1,0); //往sig_pipefd[1]上写入数据，位置为（char*)&msg,大小为1BYTE
	errno = save_errno;
}

//添加信号
void addsig(int sig,void(*handler)(int),bool restart = true)
{
	struct sigaction sa;
	memset(&sa,'\0',sizeof(sa));
	sa.sa_handler = handler;
	if(restart)
	{
		sa.sa_flags |= SA_RESTART; 
	}
	sigfillset(&sa.sa_mask); //信号处理阶段屏蔽所有新来信号
	assert(sigaction(sig,&sa,NULL) != -1); //捕获注册的信号sig，指定信号处理方式地址为&sa
}

//释放系统资源：文件描述符，共享的内存，堆内存。
void del_resource()
{
	close(sig_pipefd[0]); 
	close(sig_pipefd[1]);
	close(listenfd);
	close(epollfd);
	shm_unlink(shm_name);
	delete [] users;
	delete [] sub_process;
}

//停止一个子进程
void child_term_handler(int sig)
{
	stop_child = true;
}

//子进程运行函数。参数idx指出孩子进程处理的客户端连接的编号，
//users是保存所有客户连接数据的数组，参数share_mem指出共享内存的起始地址。
int run_child(int idx,client_data* users,char* share_mem)
{
	epoll_event events[MAX_EVENT_NUMBER];
	int child_epollfd = epoll_create(5);//创建子进程内核事件表
	assert(child_epollfd != -1);
	int connfd = users[idx].connfd;//获取子进程关联的socketfd
	addfd(child_epollfd,connfd); //向内核事件表中注册socketfd关心的事件
	int pipefd = users[idx].pipefd[1]; //获取用户打开的管道文件描述符
	addfd(child_epollfd,pipefd); //向内核事件表中注册pipefd关心的事件
	int ret;
	//子进程设置自己的信号处理函数
	addsig(SIGTERM,child_term_handler,false);//添加信号处理函数

	while(!stop_child)
	{
		int number = epoll_wait(child_epollfd,events,MAX_EVENT_NUMBER,-1);//阻塞等待内核事件表中事件的发生
		if((number<0) && (errno != EINTR))
		{
			printf("epoll failure\n");
			break;
		}

		for(int i=0; i<number; ++i)
		{
			int sockfd = events[i].data.fd;//获取事件对应的文件描述符
			//本子进程负责的客户连接有数据到达
			if((sockfd == connfd) && (events[i].events & EPOLLIN))
			{
				memset(share_mem+idx*BUFFER_SIZE,'\0',BUFFER_SIZE);
				//将客户数据读取到对应的读缓存中，该读缓存是共享内存的一段，
				//开始于idx*BUFFER_SIZE处，长度为BUFFER_SIZE字节。recv()中BUFFER_SIZE-1是为了保留一个'\0'。
				//因此，各个客户连接的读缓存是共享的。
				ret = recv(connfd,share_mem+idx*BUFFER_SIZE,BUFFER_SIZE-1,0);
				if(ret < 0)//socket数据出现错误，或者socket设置了nonblocking时没有收到数据返回-1。
				{
					if(errno != EAGAIN) //若errno==EAGAIN说明仅仅是设置了nonblocking没收到数据返回。否则代表出错。
					{
						stop_child = true;
					}
				}
				else if(ret == 0) //对方关闭了连接
				{
					stop_child = true;
				}
				else //成功读取客户数据后通知主进程（通过管道）来处理。int pipefd = users[idx].pipefd[1];
				{
					send(pipefd,(char*)&idx,sizeof(idx),0);
				}
			}

			//主进程通知本进程（通过管道）将第client个客户的数据发送到本进程负责的客户端。
			else if((sockfd == pipefd) && (events[i].events & EPOLLIN))
			{
				int client = 0;
				//接受主进程发送来的数据，即有客户数据到达的连接的编号。
				ret = recv(sockfd,(char*)&client,sizeof(client),0);
				if(ret < 0)
				{
					if(errno != EAGAIN)
					{
						stop_child = true;
					}
				}
				else if(ret == 0)
				{
					stop_child = true;
				}
				else
				{
					send(connfd,share_mem+client*BUFFER_SIZE,BUFFER_SIZE,0);
				}
			}
			else
			{
				continue;
			}
		}
	}
	close(connfd);
	close(pipefd);
	close(child_epollfd);
	return 0;
}


	

int main(int argc,char* argv[])
{
	if(argc <= 2)
	{
		printf("usage: %s ip_address prot_number \n",basename(argv[0]));
		return 1;
	}
	const char* ip = argv[1];
	int port = atoi(argv[2]);

	int ret = 0;
	struct sockaddr_in address;
	bzero(&address,sizeof(address));
	address.sin_family = AF_INET; //domain
	inet_pton(AF_INET,ip,&address.sin_addr);//ip
	address.sin_port = htons(port); //port

	listenfd = socket(PF_INET,SOCK_STREAM,0); //socket
	assert(listenfd >= 0);

	ret = bind(listenfd,(struct sockaddr*)&address,sizeof(address)); //bind
	assert(ret != -1);

	ret = listen(listenfd,5); //listen
	assert(ret != -1);

	user_count = 0;  //user number
	users = new client_data[USER_LIMIT+1]; 
	sub_process = new int[PROCESS_LIMIT];
	for(int i=0; i<PROCESS_LIMIT; ++i)
	{
		sub_process[i] = -1;
	}

	epoll_event events[MAX_EVENT_NUMBER];
	epollfd = epoll_create(5);//文件描述符epollfd标识内核事件表
	assert(epollfd != -1);
	addfd(epollfd,listenfd);//将listenfd文件描述符添加至epollfd内核事件表中
	
	ret = socketpair(PF_UNIX,SOCK_STREAM,0,sig_pipefd); //创建UNIX域socket在进程间传递辅助数据
	assert(ret != -1);
	setnonblocking(sig_pipefd[1]);
	addfd(epollfd,sig_pipefd[0]);

	addsig(SIGCHLD,sig_handler);
	addsig(SIGTERM,sig_handler);
	addsig(SIGINT,sig_handler);
	addsig(SIGPIPE,SIG_IGN);
	bool stop_server = false;
	bool terminate = false;
	
	//创建共享内存，作为所有客户socket连接的读缓存
	shmfd = shm_open(shm_name,O_CREAT | O_RDWR,0666);
	assert(shmfd != -1);
	ret = ftruncate(shmfd,USER_LIMIT*BUFFER_SIZE);
	assert(ret != -1); 

	share_mem = (char*)mmap(NULL,USER_LIMIT*BUFFER_SIZE,PROT_READ | PROT_WRITE,MAP_SHARED,shmfd,0);
	assert(share_mem != MAP_FAILED);
	close(shmfd);

	while(!stop_server)
	{
		int number = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);//等待事件发生
		if((number<0) && (errno != EINTR))
		{
			printf("epoll failure!\n");
			break;
		}

		for(int i=0; i<number; i++)
		{
			int sockfd = events[i].data.fd;
			//新的连接到来
			if(sockfd == listenfd)
			{
				struct sockaddr_in client_address;
				socklen_t client_addrlength = sizeof(client_address);
				int connfd = accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);

				if(connfd < 0)
				{
					printf("errno is: %d\n",errno);
					continue;
				}
				if(user_count >= USER_LIMIT)
				{
					const char* info = "Too many users\n";
					printf("%s",info);
					send(connfd,info,strlen(info),0);
					close(connfd);
					continue;
				}

				//保存第user_conunt个客户连接的相关数据
				users[user_count].address = client_address;
				users[user_count].connfd = connfd;
				//在主进程和子进程间建立管道，以传递必要的数据
				ret = socketpair(PF_UNIX,SOCK_STREAM,0,users[user_count].pipefd);
				assert(ret != -1);
				pid_t pid = fork();
				if(pid < 0)
				{
					close(connfd);
					continue;
				}
				else if(pid == 0) //子进程
				{
					close(epollfd);
					close(listenfd);
					close(users[user_count].pipefd[0]);
					close(sig_pipefd[0]);
					close(sig_pipefd[1]);
					run_child(user_count,users,share_mem);
					munmap((void*)share_mem,USER_LIMIT*BUFFER_SIZE);
					exit(0);
				}
				else //父进程
				{
					close(connfd);
					close(users[user_count].pipefd[1]);
					addfd(epollfd,users[user_count].pipefd[0]);
					users[user_count].pid = pid; //子进程pid
					sub_process[pid] = user_count;
					user_count++;
				}
			}

			//处理信号事件
			else if((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
			{
				int sig;
				char signals[1024];
				ret = recv(sig_pipefd[0],signals,sizeof(signals),0);
				if(ret == -1)
				{
					continue;
				}
				else if(ret == 0)
				{
					continue;
				}
				else
				{
					for(int i=0; i<ret; ++i)
					{
						switch(signals[i])
						{	
							case SIGCHLD:
								{
									pid_t pid;
									int stat;
									while((pid = waitpid(-1,&stat,WNOHANG)) > 0)
									{
										int del_user = sub_process[pid];
										sub_process[pid] = -1;
										if((del_user < 0) || (del_user > USER_LIMIT))
										{
											continue;
										}

										epoll_ctl(epollfd,EPOLL_CTL_DEL,users[del_user].pipefd[0],0);
										close(users[del_user].pipefd[0]);
										users[del_user] = users[--user_count];
										sub_process[users[del_user].pid] = del_user;
									}
									if(terminate && user_count == 0)
									{
										stop_server = true;
									}
									break;
								}
							case SIGTERM:
							case SIGINT:
								{
									printf("kill all the child now\n");
									if(user_count == 0)
									{
										stop_server = true;
										break;
									}
									for(int i=0; i<user_count; ++i)
									{
										int pid = users[i].pid;
										kill(pid,SIGTERM);
									}
									terminate = true;
									break;
								}
							default:
								{
									break;
								}
						}
					}
				}
			}
			
			//某个子进程向父进程写入了数据
			else if(events[i].events & EPOLLIN)
			{
				int child = 0;
				ret = recv(sockfd,(char*)&child,sizeof(child),0);
				printf("read data from child accross pipe\n");
				if(ret == -1)
				{
					continue;
				}
				else if(ret == 0)
				{
					continue;
				}
				else
				{
					for(int j=0; j<user_count; ++j)
					{
						if(users[j].pipefd[0] != sockfd)
						{
							printf("send data to child accross pipe\n");
							send(users[j].pipefd[0],(char*)&child,sizeof(child),0);
						}
					}
				}
			}
		}
	}
	del_resource();
	return 0;
}






















