# WebServer
基于Linux 轻量级服务器
1、使用线程池+非阻塞socket+epoll+Proactor事件处理的并发模型
2、使用有线状态机解析HTTP请求报文，支持解析GET请求
3、通过浏览器访问服务器，可以请求服务器图片、文字数据
3、经Webbebch压力测试可以实现上万的并发连接数据交换
