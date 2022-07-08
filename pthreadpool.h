#ifndef PTHREADPOOL_H
#define PTHREADPOOL_H
#include <pthread.h>
#include <list>
#include <exception>
#include <cstdio>
#include "locker.h"

// 线程池类  定义为模板类可便于代码的复用，模板参数T是任务类
template<typename T>
class threadpool {
public:
    // 构造函数 thread_number为线程池中线程的数量，m_max_requests为请求队列中最多允许的、等待处理的请求数量
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T * request);

private:
    // 工作线程运行的函数，不断从工作队列中取出任务并执行
    static void * worker(void * arg);
    void run();
    // 线程数量
    int m_thread_number;

    // 线程池数组，大小为m_thread_number
    pthread_t * m_threads;

    // 请求队列中最多允许的，等待处理的请求数量
    int m_max_requests;

    // 请求队列
    std::list< T* > m_workqueue;

    // 保护请求队列的互斥锁
    locker m_queuelocker;

    // 信号量用来判断是否有任务需要处理
    sem m_queuestat;
    
    // 是否结束线程
    bool m_stop;

};

template<typename T> 
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests),
    m_stop(false), m_threads(NULL) {
        if((thread_number <= 0) || (max_requests <= 0)) {
            throw std::exception();
        }

        m_threads = new pthread_t [m_thread_number];

        if(!m_threads) {
            throw std::exception(); // 线程池数组创建失败
        }

        // 创建thread_number 个线程，并将它们设置为脱离线程
        for(int i = 0; i < thread_number; i++) {
            printf("create the %d th thread\n", i);
            if(pthread_create(m_threads + i, NULL, worker, this) != 0) {
                // 线程创建失败 则删除线程池数组
                delete [] m_threads;
                throw std::exception();
            }
            if(pthread_detach(m_threads[i])) {
                delete [] m_threads;
                throw std::exception();
            }
        }
    }

template<typename T> 
threadpool<T>::~threadpool() {
    delete [] m_threads;
    m_stop == true;
}

template<typename T> 
bool threadpool<T>::append(T * request) {
    // 操作工作队列时一定要加锁，因为其被所有线程共享
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests) {
        // 如果请求队列中的数量超过最大承受请求数量 则不再添加
        m_queuelocker.unlock();
        return false;
    }
    // 未达到最大请求数量则可以成功添加
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;

}

// 定义线程执行的函数
template<typename T>
void * threadpool<T>::worker(void* arg) {       // worker为静态函数通过将this指针作为参数传入，从而对其内部成员进行操作
    threadpool* pool = (threadpool*) arg;
    pool->run();
    return pool;
}

template<typename T> 
void threadpool<T>::run() {
    while(!m_stop) {
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T * request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request) {
            continue;
        }
        request->process();

    }
}


#endif