#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "pthreadpool.h"
#include "http_conn.h"

/*
    代码整体逻辑
    1、信号捕捉-》 对SIGPIPE信号进行忽略 
    =============================================================================================================
        **SIGPIPE为何会产生？**
        简单来说，就是客户端程序向服务器端程序发送了消息，然后关闭客户端，服务器端返回消息的时候就会收到内核给的SIGPIPE信号。
        ”’对一个已经收到FIN包的socket调用read方法, 如果接收缓冲已空, 则返回0, 
        这就是常说的表示连接关闭. 但第一次对其调用write方法时, 如果发送缓冲没问题, 
        会返回正确写入(发送). 但发送的报文会导致对端发送RST报文, 因为对端的socket
        已经调用了close, 完全关闭, 既不发送, 也不接收数据. 所以, 第二次调用write方
        法(假设在收到RST之后), 会生成SIGPIPE信号, 导致进程退出.”
    =============================================================================================================
    因此需要对SIGPIPE信号进行忽略
    
    =============================================================================================================
    2、设置线程池-》 
            a、在线程池中定义了线程池队列，append()函数，可向队列添加需要处理的线程
            b、线程池中线程创建后，执行工作线程的逻辑代码worker()
            c、在工作线程的逻辑代码中调用run()函数，该函数以此遍历线程池队列，并调用process()函数处理每个线程的请求和应答
    =============================================================================================================


    =============================================================================================================
    在工作线程中，操作工作队列时一定要加锁，因为其被所有线程共享。操作之后需要进行解锁。同时设置semaphore信号量，用来判断是
    否有任务需要处理。 如果没有需要处理的任务，则阻塞，直到有任务需要处理。
    =============================================================================================================

    =============================================================================================================
    3、设置监听套接字，并使用epoll在内核中创建对象实例，将要检测的文件描述符添加至就绪队列，并设置为阻塞模式，有文件描述符发
    生变化，则返回其变化的文件描述符数量以及发生了那些事件
        -》 如果发现文件描述符中有数据写入，则一次性读出，并交由线程进行解析和应答处理。（步骤2中的b、c）
        -》之后再次检测，是否可写，如果可也交由线程进行处理（主要为应答 服务器向客户端发送数据）
    =============================================================================================================


*/
#define MAX_FD 65535            // 最大文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大事件数量

extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);


// 添加信号捕捉
void addsignal(int sig, void(handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}


int main(int argc, char* argv[]) {
    // argc为是命令行总的参数个数  
    // argv[]是argc个参数，第0个参数是程序的全名，之后是用户输入的参数

    if(argc <= 1) {
        printf("按照如下格式运行： %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    int port = atoi(argv[1]);
    addsignal(SIGPIPE, SIG_IGN);

    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    } catch(...) {
        return 1;
    }

    http_conn* users = new http_conn[MAX_FD];
    // 设置监听套接字
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    int ret = 0;
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // 设置端口复用
    int reuse = 1;  // 1 表示复用 0 表示不复用
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // 绑定
    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    // 设置监听
    ret = listen(listenfd, 5); 
    
    // 创建epoll对象和事件数组 
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    // 将监听套接字添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd; // 所有的socket上的事件都被注册到同一个epollfd中

    while(true) {
        // 返回发生变化的文件描述符个数 并设置为阻塞模式
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        // 当捕捉到信号后，进行处理，产生中断。当中断返回时，则产生EINTR错误
        if((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        for(int i = 0; i < number; i++) {
            // printf("i: %d, number: %d\n", i, number);
            // 依次处理发生变化的文件描述符
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd) {  
                // 如果为监听文件描述符，则表示有新的客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                // 获取客户端的文件描述符及信息
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);

                if(connfd < 0) {
                    printf("errno is %d \n", errno);
                    continue;
                }

                if(http_conn::m_user_count >= MAX_FD) {
                    // 目前连接数满了
                    close(connfd);
                    continue;
                }

                // 将新的客户的数据初始化，放至到数组中 
                // 初始化函数中 设置了端口复用以及客户端文件描述符监听
                users[connfd].init(connfd, client_address); 

            } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 对方异常断开或者发生错误时间
                users[sockfd].close_conn();

            } else if(events[i].events & EPOLLIN) {
                // 有数据写入 则将其一次性全部读出
                if(users[sockfd].read()) {
                    pool->append(users + sockfd);   // 将其加入到线程池中
                } else {
                    users[sockfd].close_conn();
                }

            } else if(events[i].events & EPOLLOUT) {
                // 检测是否有空间写入 一次性写完所有数据
                if(!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }
            }

        }
    }
    

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;

    return 0;
}

