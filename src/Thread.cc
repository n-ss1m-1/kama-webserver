#include <Thread.h>
#include <CurrentThread.h>

#include <semaphore.h>

//静态成员初始化
std::atomic_int Thread::numCreated_(0);

/**
 * 构造函数：资源初始化但不启动线程
 * 设计理念：延迟初始化(构造时并不会立即创建底层线程)，真正需要时才创建系统线程
 * 创建Thread对象->调用start方法，从而真正启动一个包含EventLoop的新线程，
 * 遵循着 "one loop per thread"​ 的设计原则
 */
Thread::Thread(ThreadFunc func, const std::string &name)
    : started_(false)
    , joined_(false)
    , tid_(0)
    , func_(std::move(func))
    , name_(name)
{
    ++numCreated_;
    setDefaultName();
}

/**
 * 析构函数：RAII自动资源清理
 * 策略选择：started但未joined → detach（守护线程）
 * 设计考虑：避免线程对象销毁时资源泄漏
 */
Thread::~Thread()
{
    if (started_ && !joined_)
    {
        thread_->detach();                                                  // thread类提供了设置分离线程的方法 线程运行后自动销毁（非阻塞）
    }
}

/**
 * 核心方法：创建并启动一个子线程
 * 线程安全：通过信号量确保tid的正确获取
 * 设计亮点：解决父子线程间的启动同步问题
 */
void Thread::start()   //父线程调用
{
    started_ = true;
    sem_t sem;
    sem_init(&sem, false, 0);   // false指的是 不设置进程间共享  sem值初始化为0
    // 创建子线程
    thread_ = std::shared_ptr<std::thread>(new std::thread([&]() {
        tid_ = CurrentThread::tid();    // 获取子线程的tid值(子线程通过 [&]捕获tid_ 修改为自己的线程ID 再传递出去)
        sem_post(&sem);                 // 子线程创建完毕->通过信号量通知父线程
        func_();                        // 子线程的回调函数->EventLoopThread::threadFunc() 创建相应的eventloop
    }));

    // 这里父线程必须等待获取上面子线程的tid值
    sem_wait(&sem);
    sem_destroy(&sem);  // 清理信号量资源
}
/*
为什么这里父线程需要信号量等待：
1.如果在子线程执行 tid_ = CurrentThread::tid()之前，父线程（或其他线程）尝试读取或使用 tid_成员变量，那么读到的将是一个未初始化的、无效的值（可能是0，也可能是垃圾值）。这会导致程序行为不可预测，甚至崩溃。
2.信号量比较简单轻量 适合这样的一次性通信
*/

// C++ std::thread 中join()和detach()的区别：https://blog.nowcoder.net/n/8fcd9bb6e2e94d9596cf0a45c8e5858a
//等待线程结束
void Thread::join()
{
    joined_ = true;
    thread_->join();
}

void Thread::setDefaultName()
{
    if (name_.empty())
    {
        char buf[32] = {0};
        snprintf(buf, sizeof buf, "Thread%d", numCreated_.load());  //用 .load() 方法安全地读取原子变量的当前值
        name_ = buf;
    }
}

/*
区分    父线程 (Main Reactor)​                                            子线程 (Sub Reactor)​
    
角色:
​程序的启动线程，通常是 main()函数所在线程。                                由父线程动态创建的工作线程。
它晋升为网络库的主事件循环。

核心职责​:
1. 负责监听新连接请求（通过 Acceptor）。                                   1. 处理已建立连接的I/O事件（读写）。
2. 接收新连接后，分发给子线程池。                                          2. 执行业务逻辑回调。


包含组件​   一个TcpServer(内含一个 Acceptor + 一个EventLoopThreadPool)      一个独立的 EventLoop对象。

创建时机​   程序启动时自然存在。                                            在 TcpServer::start()被调用后，由父线程通过 EventLoopThreadPool按需创建。

通信方式​   通过 eventfd和任务队列向子线程提交任务（如分发新连接）。          处理来自父线程的任务，并通过网络与客户端通信。

*/