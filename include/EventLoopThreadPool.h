#pragma once

#include <functional>
#include <string>
#include <vector>
#include <memory>

#include "noncopyable.h"
class EventLoop;
class EventLoopThread;

/*
与其他类的关系：
1.与EventLoop：
    负责管理EventLoop的创建、启动、销毁
    提供统一接口来获取和操作EventLoop
2.与EventLoopThread：
    工厂模式：使用EventLoopThread作为工厂来创建EventLoop
    委托模式：将线程管理委托给EventLoopThread 专注于池化管理
3.与TcpServer：
    为TcpServer提供服务
*/

class EventLoopThreadPool : noncopyable
{
public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg);
    ~EventLoopThreadPool();

    //设置线程池大小(0表示单线程模式)(必须在start前调用)
    void setThreadNum(int numThreads) { numThreads_ = numThreads; }

    //启动线程池
    void start(const ThreadInitCallback &cb = ThreadInitCallback());

    // 如果工作在多线程中，baseLoop_(mainLoop)会默认以轮询的方式分配Channel给subLoop
    EventLoop *getNextLoop();

    // 获取所有的EventLoop
    std::vector<EventLoop *> getAllLoops(); 

    bool started() const { return started_; }           // 是否已经启动
    const std::string name() const { return name_; }    // 获取名字

private:
    EventLoop *baseLoop_;   // main EventLoop：Acceptor所在循环
    std::string name_;      // 线程池名称，通常由用户指定，线程池中EventLoopThread名称依赖于线程池名称。
    bool started_;          // 是否已经启动的标志
    int numThreads_;        // 线程池中线程的数量
    int next_;              // 新连接到来，所选择EventLoop的索引
    std::vector<std::unique_ptr<EventLoopThread>> threads_;     // IO线程的列表
    std::vector<EventLoop *> loops_;    // 线程池中EventLoop的列表，指向的是EVentLoopThread的线程函数创建的EventLoop对象。
};