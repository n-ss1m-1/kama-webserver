#include <memory>

#include <EventLoopThreadPool.h>
#include <EventLoopThread.h>
#include <Logger.h>
EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg)
    : baseLoop_(baseLoop)
    , name_(nameArg)
    , started_(false)
    , numThreads_(0)
    , next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool()
{
    // 使用unique_ptr自动管理资源，无需手动delete (std::vector<std::unique_ptr<EventLoopThread>> threads_;)
    // 当threads_向量销毁时，会自动释放所有EventLoopThread对象
    // subLoop创建在子线程栈空间上,离开栈空间自动销毁
}

void EventLoopThreadPool::start(const ThreadInitCallback &cb)
{
    //标记
    started_ = true;

    //多线程模式：创建指定数量的EventLoopThread(使用unique_ptr自动管理)
    for (int i = 0; i < numThreads_; ++i)
    {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);        //线程名称：线程池名+序号i
        EventLoopThread *t = new EventLoopThread(cb, buf);          
        threads_.push_back(std::unique_ptr<EventLoopThread>(t));    //=threads_.emplace_back(cb,buf)
        loops_.push_back(t->startLoop()); // 底层创建线程 初始化eventloop并绑定 然后返回该loop的地址
    }

    if (numThreads_ == 0 && cb) // 整个服务端只有一个线程运行baseLoop
    {
        // 如果设置了回调且为单线程模式，对baseLoop执行初始化？
        cb(baseLoop_);
    }
}


/**
 * ⭐⭐⭐ 核心方法：轮询获取下一个subLoop 分配Channel（多线程模式下的负载均衡）
 * 
 * 轮询算法特点：
 * - 简单高效，O(1)时间复杂度
 * - 均匀分配，避免单个EventLoop过载
 * - 无锁操作，next_仅在主线程修改，线程安全
 */
EventLoop *EventLoopThreadPool::getNextLoop()
{
    // 如果只设置一个线程 也就是只有一个mainReactor 无subReactor 
    // 那么轮询只有一个线程 getNextLoop()每次都返回当前的baseLoop_
    EventLoop *loop = baseLoop_;    

    // 通过轮询获取下一个处理事件的loop
    if(!loops_.empty())             
    {
        loop = loops_[next_];
        ++next_;
        // 轮询
        if(next_ >= loops_.size())
        {
            next_ = 0;
        }
    }

    return loop;
}


std::vector<EventLoop *> EventLoopThreadPool::getAllLoops()
{
    if (loops_.empty())     //特殊处理单线程模式：返回baseLoop的向量
    {
        return std::vector<EventLoop *>(1, baseLoop_);
    }
    else        //多线程模式：返回所有subLoop
    {
        return loops_;      
    }
}


/*
从EventLoopThreadPoll::start()开始到结束的流程(父线程) 以及子线程创建的时刻到开始工作的流程：
父：
EventLoopThreadPool::start()->EventLoopThread::startLoop()->thread_.start()->sem_wait(&sem)->cond_.wait()->return loop->loops_.push_back()
子：
Thread::start()中thread_ = std::shared_ptr<std::thread>(new std::thread([&](){ }));->EventLoopThread::threadFunc()->cond_.notify_one()->loop.loop()
*/