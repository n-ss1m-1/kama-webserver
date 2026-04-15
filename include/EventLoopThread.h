#pragma once

#include <functional>
#include <mutex>
#include <condition_variable>
#include <string>

#include "noncopyable.h"
#include "Thread.h"

class EventLoop;

/*
与其他类的关系：
1.与EventLoop：
    EventLoopThread负责创建和管理 EventLoop的生命周期
    实现线程绑定：One Loop Per Thread
    访问控制：组合loop 获取相应的接口
2.与Thread：组合模式
    职责委托：Thread类具备线程管理功能
    功能拓展：在此基础上 实现事件循环的管理能力
*/

/**
 * EventLoopThread - 事件循环线程类
 * 
 * 核心功能：将EventLoop与Thread绑定，实现"One Loop Per Thread"架构
 * 设计模式：生产者-消费者模式（主线程生产EventLoop，子线程消费运行）
 * 线程安全：通过互斥锁和条件变量确保跨线程安全
 */
class EventLoopThread : noncopyable
{
public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    EventLoopThread(const ThreadInitCallback &cb = ThreadInitCallback(),
                    const std::string &name = std::string());
    ~EventLoopThread();

     /**
     * 核心方法：启动事件循环线程
     * @return 指向新创建EventLoop的指针（线程安全）
     * 
     * 工作流程：
     * 1. 启动底层线程
     * 2. 等待新线程完成EventLoop初始化
     * 3. 返回初始化完成的EventLoop指针
     * 
     * 注意：此方法会阻塞直到EventLoop在新线程中完全初始化
     */
    EventLoop *startLoop();

private:
    /**
     * 线程函数：在新线程中执行
     * 核心职责：
     * 1. 创建EventLoop对象（线程局部）
     * 2. 执行用户初始化回调
     * 3. 通知主线程EventLoop已就绪
     * 4. 启动事件循环
     */
    void threadFunc();

    EventLoop *loop_;               // one loop per thread 
    Thread thread_;                 // 第二层的线程管理对象
    std::mutex mutex_;              // 互斥锁，保护loop_指针的访问(？)
    std::condition_variable cond_;  // 条件变量，用于线程间同步?
    ThreadInitCallback callback_;   // 线程初始化回调函数
    bool exiting_;                  // 退出标志，用于安全关闭
};