#include "http_conn.h"
// 定义HTTP响应的一些状态信息
const char * ok_200_title = "OK";
const char * error_400_title = "Bad Request";
const char * error_400_form = "Your request has bad syntax or is inherently impossible to satissfy.\n";
const char * error_403_title = "Forbidden";
const char * error_403_form = "You do not have permission to get file from this server.\n";
const char * error_404_title = "Not Found";
const char * error_404_form = "The requested file was not found on this server.\n";
const char * error_500_title = "Internal Error";
const char * error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char * doc_root = "/home/gsq/文档/linux_cpp/webserver/resources";




int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;
// 设置文件描述符非阻塞
void setnonblocking(int fd) {
    // fcntl为可对文件描述符进行操作  根据不同命令执行不同操作
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

// 向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if(one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd); 
}

// 修改文件描述符 重置socket上的EPOLLONESHOT事件，确保下一次可读时，EPOLLIN事件被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void http_conn::init(int sockfd, const sockaddr_in & addr) {
    m_socket = sockfd;
    address = addr;
    // 端口复用
    int reuse = 1;  // 1 表示复用 0 表示不复用
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++; // 总用户数+1

    init();
}


void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始化状态为解析请求首行
    m_checked_index = 0;
    m_start_line = 0;
    m_read_index = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;
    m_content_length = 0;
    m_write_index = 0;
    m_host = 0; 

    bzero(m_read_buffer, READ_BUFFER_SIZE);     // 读缓冲清空
    bzero(m_write_buffer, WRITE_BUFFER_SIZE);   // 写缓存清空
    bzero(m_real_file, FILENAME_LEN);           // 文件路径 
}

void http_conn::close_conn() {
    if(m_socket != -1) {
        removefd(m_epollfd, m_socket);
        m_socket = -1;
        m_user_count--; // 关闭一个连接 客户总数量需要对应减少
    }
}


// 循环读取客户数据，直到无数据刻度或者对方关闭连接
bool http_conn::read() {
    // printf("一次性读出所有数据\n");
    if(m_read_index >= READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;
    while(true) {
        bytes_read = recv(m_socket, m_read_buffer + m_read_index, READ_BUFFER_SIZE - m_read_index, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据可读
                break;
            }
            return false;
        } else if(bytes_read == 0) {
            // 对方关闭连接
            return false;
        }
        m_read_index += bytes_read;
    }
    printf("读取到了数据： %s\n", m_read_buffer);
    return true;
}


http_conn::HTTP_CODE http_conn::do_request() {
    /*
        当得到一个完整的、正确的http请求时，分析目标文件的属性，如果目标文件存在、多所有用户可读且不是目录。
        则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功。
    */
   strcpy(m_real_file, doc_root);   // 先获取根目录
   int len = strlen(doc_root);
   strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);   // 从m_url复制FILENAME_LEN - len - 1个字符
   // 获取m_real_file 文件相关的状态信息 -1表示失败 0 表示成功
   // 函数说明: 通过文件名filename获取文件信息，并保存在buf所指的结构体stat中
   if(stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if(!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;   // 客户对资源没有足够的访问权限
    }

    // 判断是否是目录
    if(S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 以只读方式打开
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射  将保存在m_file_address内存地址位置的数据发送给客户
    m_file_address = (char*) mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;    // 文件请求,获取文件成功

}

// 对内存映射区执行munmap操作 解除地址映射
void http_conn::unmap() {
    if(m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}



// 从主状态机中解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    // 定义初始状态
    LINE_STATUS line_statue = LINE_OK;
    HTTP_CODE ret = NO_REQUEST; // 最终解析的结果
    char * text = 0;            // 存储获取一行的数据

    while(((m_check_state == CHECK_STATE_CONTENT) && (line_statue == LINE_OK)) 
            ||((line_statue = parse_line()) == LINE_OK)) {
        // 检测到请求体的同时行状态为ok 可进行解析 或者检测到请求行状态为ok
        // 解析到一行完整的数据 或者解析到了请求体，也是完整的数据

        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_index;
        printf("got 1 http line : %s\n", text);
        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE: {  // 当前正在分析请求行
            
                ret = parse_request_line(text); // 
                if(ret == BAD_REQUEST) {    // 语法错误则直接结束
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {       // 当前正在分析头部字段
            
                ret = parse_heders(text);
                if(ret = BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if(ret == GET_REQUEST) {
                    return do_request();    // do_request为解析具体的请求信息
                }
                break;
            }
            case CHECK_STATE_CONTENT: {   //当前正在解析请求体
            
                ret = parse_content(text);
                if(ret == GET_REQUEST) {    //获得一个完整的请求
                    return do_request();    
                }
                line_statue = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
        return NO_REQUEST;      // 请求不完整 需要重新获取客户信息
    }

    return NO_REQUEST;
}

// 解析http请求行，获取请求方法，目标URL HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    // GET /index.html HTTP/1.1 
    m_url = strpbrk(text, " \t");   // strbrk()方法 返回指向str1中第一次出现的作为str2一部分的任何字符的指针，没有匹配则返回空指针
    if(!m_url) {
        return BAD_REQUEST;
    }

    *m_url++ = '\0';               // 置\0之后再向后移动一个位置 此时指向index之前的/
    
    // GET\0/index.html HTTP/1.1 
    char * method = text;   // 字符串只取到 \0 处 遇到\0就停止了
    if(strcasecmp(method, "GET") == 0) {        // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1 
    m_version = strpbrk(m_url, " \t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';

    // /index.html\0HTTP/1.1 
    if(strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    if(strncasecmp(m_url, "http://", 7) == 0) {  // 此时m_url为  /index.html\0
        m_url += 7;     // 如果是以http://打头的  则向后移动7个位置
        m_url = strpbrk(m_url, '/');
    }

    if(!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER; // 走到此处 则表明请求行解析完成 将主状态机设置为检车请求头


    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_heders(char *text) {
    // 如果遇到空行，表示头部字段解析完毕
    if(text[0] == '\0') {
        // 如果http请求有消息体，则还需要读取m_content_length 字节的消息体
        // 状态机转移到CHECK_STATE_CONTENT （解析请求体状态）
        if(m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明已经得到一个完整的HTTP请求
        return GET_REQUEST;
    } else if(strncasecmp(text, "Connection:", 11) == 0) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;         
        text += strspn(text, " \t");    // strspn() 扫描str1以查找属于str2的任何字符的第一次出现，返回在第一次出现之前读取的str1的字符数。
        if(strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }

    } else if(strncasecmp(text, "Content-Length:", 15) == 0) {
        // 处理请求体Content-Length字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);  // string 转为 int

    } else if(strncasecmp(text, "Host:", 5) == 0) {

        text += 5;
        text += strspn(text, " \t");
        m_host = text;

    } else {
        printf("oop! unknow header %s\n", text);
    }

    return NO_REQUEST;
}

// 在此并未真正解析http请求的消息体，只是判断是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    if(m_read_index >= (m_content_length + m_checked_index)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}


// 解析一行 判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for(; m_checked_index < m_read_index; ++m_checked_index) {
        temp = m_read_buffer[m_checked_index];
        if(temp == '\r') {
            if((m_checked_index + 1) == m_read_index) {
                return LINE_OPEN;
            } else if(m_read_buffer[m_checked_index + 1] == '\n') {
                m_read_buffer[m_checked_index++] = '\0';    // 将后面的 \r\n 全部改为 \0
                m_read_buffer[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if(temp == '\n') {
            if((m_checked_index > 1) && (m_read_buffer[m_checked_index - 1] == '\r')) {
                m_read_buffer[m_checked_index - 1] = '\0';
                m_read_buffer[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}


// 写http响应
bool http_conn::write() {
    // printf("一次性写入所有数据\n");
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_index; // 将要发送的字节  m_write_index为写缓冲区中待发送的字节数
    
    if(bytes_to_send == 0) {
        // 没有待发送的字节
        modfd(m_epollfd, m_socket, EPOLLIN);
        init();
        return true;
    }
    while(1) {
        // 分散写 两块要写入的内存数据 一块为响应头数据， 一块为响应体数据
        temp = writev(m_socket, m_iv, m_iv_count);  // 全部写入
        if(temp <= -1) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即收到同一个客户的下一个请求，但可以保证连接的完整性
            if(errno == EAGAIN) {
                modfd(m_epollfd, m_socket, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;  // 因为是全部写入 则写入之后待发送的字节数必然小于已发送的字节数
        bytes_have_send += temp;
        if(bytes_to_send <= bytes_have_send) {
            // 发送http响应成功，根据http请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {
                init();
                modfd(m_epollfd, m_socket, EPOLLIN);
                return true;
            } else {
                modfd(m_epollfd, m_socket, EPOLLIN);
                return false;
            }
        }
    }
}



// 向写缓冲区写入待发送的数据
bool http_conn::add_response(const char * format, ...) {
    if(m_write_index >= WRITE_BUFFER_SIZE) {
        return false;
    }
    va_list arg_list;   // 用于解析参数 参数列表
    va_start(arg_list, format); // 
    int len = vsnprintf(m_write_buffer + m_write_index, WRITE_BUFFER_SIZE - 1 - m_write_index, format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_index)) {
        return false;
    }
    m_write_index += len;
    va_end(arg_list);
    return true;

}


bool http_conn::add_status_line(int status, const char * title) {  // 添加响应状态首行
    return add_response("%s %d %s \r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_length) {                  // 增加响应头
    add_content_length(content_length);
    add_content_type();
    add_linger();
    add_blank_line();
}


bool http_conn::add_content_type() {
    return add_response("Content-Type: %s\r\n", "text/html");
}

bool http_conn::add_content_length(int content_length) {
    return add_response("Content-Length: %d\r\n", content_length);
}

bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char * content) {
    return add_response("%s", content);
}


// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
    case INTERNAL_ERROR:
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if(!add_content(error_500_form)) {
            return false;
        }
        break;
    case BAD_REQUEST:
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if(!add_content(error_400_form)) {
            return false;
        }
        break;
    
    case FORBIDDEN_REQUEST: // 没有访问权限
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if(!add_content(error_404_form)) {
            return false;
        }
        break;
    case FILE_REQUEST:  // 获取文件成功
        add_status_line(200, ok_200_title);
        add_headers(m_file_stat.st_size);
        m_iv[0].iov_base = m_write_buffer;
        m_iv[0].iov_len = m_write_index;
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count = 2;
        return true;
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buffer;
    m_iv[0].iov_len = m_write_index;
    m_iv_count = 1;
    return true;
}


void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST) {
        // 如果请求不完整还需要继续读取数据 则修该监听事件 重新监听
        modfd(m_epollfd, m_socket, EPOLLIN);
        return;
    }

    // printf("parse requese, create response\n");
    // 生成响应
    bool write_ret = process_write(read_ret);
    if(!write_ret) {
        close_conn();
    }
    // 因为使用了oneshot 只监听一次，因此写成功后还需将写时间重新添加到监听中
    modfd(m_epollfd, m_socket, EPOLLOUT);

}