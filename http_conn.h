#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>
#include <string.h>


class  http_conn {
public:
    static int m_epollfd; // 所有的socket上的事件都被注册到同一个epollfd中
    static int m_user_count; // 统计用户的数量
    static const int READ_BUFFER_SIZE = 2048; // 读缓存大小
    static const int WRITE_BUFFER_SIZE = 1024; // 写缓存大小
    static const int FILENAME_LEN = 200;

    // http 请求方法 这里仅支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};

    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */

    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };

    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    
    // 从状态机的三种可能状态，即 行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };



    http_conn() {}
    ~http_conn() {};
    void process(); // 处理客户端请求 
    void init(int sockfd, const sockaddr_in & addr); // 初始化新接收的连接
    void close_conn();
    bool read();        // 非阻塞读数据
    bool write();       // 非阻塞写数据

    // 以下被process_read调用用于分析http请求
    HTTP_CODE process_read();               // 解析http请求
    HTTP_CODE parse_request_line(char *text);       // 解析请求首行
    HTTP_CODE parse_heders(char *text);     // 解析请求头
    HTTP_CODE parse_content(char *text);    // 解析请求体

    LINE_STATUS parse_line();               // 解析每行的信息并分送到相应的处理函数中(如请求首行、请求头、请求体)
    HTTP_CODE do_request();

    // 这一组函数被process_write调用以填充http应答
    void unmap();
    bool add_response(const char * format, ...); // ...变长度参数 向写缓冲区中发送数据
    bool add_content(const char * content);
    bool add_content_type();
    bool add_status_line(int status, const char * title);   // 添加响应状态首行
    bool add_headers(int content_length);                   // 增加响应头
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    bool process_write(HTTP_CODE ret); // 响应http请求




private:
    int m_socket; // 该http连接的socket
    sockaddr_in address;
    char m_read_buffer[READ_BUFFER_SIZE];   // 读缓冲区
    int m_read_index;           // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置    

    int m_checked_index;        // 当前读缓冲区中正在分析的字符所处的位置
    int m_start_line;           // 当前正在解析的行的起始位置
    CHECK_STATE m_check_state;  // 主状态机当前所处的状态
    char * m_url;               // 客户请求的目标文件的文件名
    char * m_version;           // HTTP协议版本号，我们仅支持HTTP1.1
    METHOD m_method;            // 请求方法
    char * m_host;              // 主机名
    bool m_linger;              // HTTP请求是否要求保持连接
    int m_content_length;       // 请求体字节数
    char m_real_file[FILENAME_LEN]; // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录


    char m_write_buffer[WRITE_BUFFER_SIZE];    // 写缓冲区
    int m_write_index;            // 写缓冲区中待发送的字节数
    char* m_file_address;       // 客户请求的目标文件被mmap到内存中的起始
    struct stat m_file_stat; // 目标文件的状态 通过该状态可以判断文件是否存在、是否为目录、是否可读、并获取文件大小等信息
    struct iovec m_iv[2];       // 采用writev来执行写操作，所有定义以下两个成员，其中m_iv_count表示被写内存块的数量
    int m_iv_count;



    void init(); // 初始化连接的一些信息

    char * get_line() { return m_read_buffer + m_start_line; }

};



#endif