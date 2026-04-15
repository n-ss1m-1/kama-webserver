#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>

#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"
#include "TimerQueue.h"
class Channel;
class Poller;

// 事件循环类 主要包含了两个大模块 Channel Poller(epoll的抽象)
class EventLoop : noncopyable
{
public:
    using Functor = std::function<void()>;  //只接受[无参+无返回值]的可调用对象

    EventLoop();
    ~EventLoop();

    // 开启事件循环
    void loop();
    // 退出事件循环
    void quit();

    Timestamp pollReturnTime() const { return pollRetureTime_; }

    // 欲在当前loop中执行cb(保证线程安全)
    void runInLoop(Functor cb);
    // 把cb放入任务队列中 唤醒loop所在的线程执行cb
    void queueInLoop(Functor cb);

    // 通过eventfd唤醒loop所在的线程
    void wakeup();

    // EventLoop的方法 => Poller的方法
    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);
    bool hasChannel(Channel *channel);

    /* 
    判断EventLoop对象是否在自己的线程里
    别的线程可能调用任意eventLoop(如：MainLoop向SubLoop分发新连接) 但每个EventLoop和它管理的所有Channel都应该是和相应线程绑定的 -> 避免数据竞争
    threadId_为EventLoop创建时的线程id CurrentThread::tid()为当前运行的线程id  
    */
    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }
    /**
     * 定时任务相关函数
     */
    void runAt(Timestamp timestamp, Functor &&cb)
    {
        timerQueue_->addTimer(std::move(cb), timestamp, 0.0);
    }

    void runAfter(double waitTime, Functor &&cb)
    {
        Timestamp time(addTime(Timestamp::now(), waitTime));
        runAt(time, std::move(cb));
    }

    void runEvery(double interval, Functor &&cb)
    {
        Timestamp timestamp(addTime(Timestamp::now(), interval));
        timerQueue_->addTimer(std::move(cb), timestamp, interval);
    }

private:
    void handleRead();        // 给eventfd返回的文件描述符wakeupFd_绑定的事件回调 当wakeup()时-即有事件发生时 唤醒阻塞的epoll_wait 调用handleRead()读wakeupFd_的8字节 同时可用处理任务队列中的任务doPendingFunctors()
    void doPendingFunctors(); // 执行加入任务队列中的任务

    using ChannelList = std::vector<Channel *>;

    std::atomic_bool looping_; // 标识是否正在事件循环 原子操作 底层通过CAS实现
    std::atomic_bool quit_;    // 标识退出loop循环

    const pid_t threadId_; // 记录当前EventLoop是被哪个线程id创建的 即标识了当前EventLoop的所属线程id

    Timestamp pollRetureTime_; // Poller返回发生事件的Channels的时间点
    std::unique_ptr<Poller> poller_;    //监听事件(mainloop:连接请求事件 subloop:读写等事件)
    const int kPollTimeMs_;         //epoll_wait()超时时间
    std::unique_ptr<TimerQueue> timerQueue_;
    int wakeupFd_; // 作用：当mainLoop获取一个新用户的Channel 需通过轮询算法选择一个subLoop 通过该成员唤醒subLoop处理Channel
    std::unique_ptr<Channel> wakeupChannel_;    // wakeup封装为channel 注册到自己的poller中 关注可读事件 -> 用于在任务队列中需要执行时 解开阻塞的poll()

    ChannelList activeChannels_; // 返回Poller检测到当前有事件发生的所有Channel列表

    std::atomic_bool callingPendingFunctors_; // 标识当前loop是否有正在执行的线程间的内部任务
    std::vector<Functor> pendingFunctors_;    // 任务队列 存储当前loop需要执行的所有回调操作(除读写回调等)
    std::mutex mutex_;                        // 互斥锁 用来保护上面任务队列的线程安全操作
};

/*
设计原则：One Loop Per Thread + One Poller Per Loop
为什么一个loop需要一个poller，而不能共享poller：
1.共享poller->ctl操作会产生严重的锁竞争，而独享poller无需加锁
2.线程通信机制(eventfd+任务队列->解除poller_wait阻塞->唤醒当前loop的所属线程)决定的，需要精确的唤醒机制(one poller-one pthread-one loop)
3.封装需要，eventloop封装poller方法，供channel使用

不要通过共享内存来通信，要通过通信来共享内存。
通过任务队列和eventfd进行线程间通信，而不是共享Poller
*/