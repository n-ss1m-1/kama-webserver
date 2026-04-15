#include <EventLoopThread.h>
#include <EventLoop.h>

EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,
                                 const std::string &name)
    : loop_(nullptr)    //初始化为空 在子线程中创建
    , thread_(std::bind(&EventLoopThread::threadFunc, this), name)  //子线程的回调函数
    , mutex_()
    , cond_()
    , callback_(cb)     //用户初始化回调
    , exiting_(false)
{
}

EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    if (loop_ != nullptr)
    {
        loop_->quit();
        thread_.join();
    }
}

/**
 * ⭐⭐⭐ 核心方法：启动事件循环线程（线程安全）
 * 
 * 这个方法解决了经典的竞态条件问题：->实现线程同步
 * - 主线程需要获取在新线程中创建的EventLoop指针
 * - 必须确保EventLoop完全初始化后才能返回
 */
EventLoop *EventLoopThread::startLoop()
{
    thread_.start(); // 主线程启动子线程(并让子线程调用EventLoopThread::threadFunc() 初始化eventloop)

    EventLoop *loop = nullptr;
    {
        //使用条件变量 等待eventloop初始化完成
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this](){return loop_ != nullptr;});
        loop = loop_;
    }
    return loop;        //安全返回eventloop指针
}

/**
 * ⭐⭐⭐ 线程函数：在新线程中执行（核心中的核心）
 * 注意：此函数决定了"One Loop Per Thread"架构的实现
 */
void EventLoopThread::threadFunc()
{
    EventLoop loop(1); // 1.创建子线程的loop

    if (callback_)  //2.执行用户初始化的回调(若设置)
    {
        callback_(&loop);
    }

    //3.通知主线程
    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;          //让主线程获取相应loop
        cond_.notify_one();     //条件变量：唤醒主线程
    }

    // 4.执行EventLoop的loop() 开启了底层的Poller的poll()
    loop.loop();    

    //5.清理工作
    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = nullptr;
}