#include <functional>
#include <string.h>

#include <TcpServer.h>
#include <Logger.h>
#include <TcpConnection.h>

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL<<"main Loop is NULL!";
    }
    return loop;
}

//构造函数：初始化成员变量 设置acceptor和TcpConnection的回调函数
TcpServer::TcpServer(EventLoop *loop,
                     const InetAddress &listenAddr,
                     const std::string &nameArg,
                     Option option)
    : loop_(CheckLoopNotNull(loop))
    , ipPort_(listenAddr.toIpPort())
    , name_(nameArg)
    , acceptor_(new Acceptor(loop, listenAddr, option == kReusePort))
    , threadPool_(new EventLoopThreadPool(loop, name_))
    , connectionCallback_()
    , messageCallback_()
    , nextConnId_(1)
    , started_(0)
{
    // 当有新用户连接时，Acceptor类中绑定的acceptChannel_会有读事件发生，执行Acceptor::handleRead() 在其内调用TcpServer设置的回调函数Acceptor::NewConnectionCallback_
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this, std::placeholders::_1, std::placeholders::_2));
}

TcpServer::~TcpServer()
{
    for(auto &item : connections_)
    {
        TcpConnectionPtr conn(item.second);
        item.second.reset();    // 把原始的智能指针复位 让栈空间的TcpConnectionPtr conn指向该对象 当conn出了其作用域 即可释放智能指针指向的对象
        // 销毁连接
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, conn));
    }
}

// 设置底层subloop的个数
void TcpServer::setThreadNum(int numThreads)
{
    numThreads_=numThreads;
    threadPool_->setThreadNum(numThreads_);
}

/*
开启服务器监听：
先启动线程池：防止先启动监听+新连接快速到来->无法找到可用的subLoop
再启动监听器(在主线程中执行)：因为acceptor属于TcpServer TcpServer属于mainloop 为保证线程安全 必须由mainloop执行(listen()的acceptChannel调用enableReading() 这需要在主线程对应的poller_上设置监听事件 不能设置到其他eventLoop上)
*/
void TcpServer::start()
{
    if (started_.fetch_add(1) == 0)    // 防止一个TcpServer对象被start多次(原子操作)    实际：加锁 - 读取旧值(0) - 原值+1 - 返回旧值
    {
        threadPool_->start(threadInitCallback_);    // 启动底层的loop线程池
        loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get()));    //Acceptor::listen()启动监听 监听新连接的到来事件
    }                                                   //对比：始终在一个线程，不会被其他线程突然销毁，与TcpServer同生共死 使用.get()是安全的
}

/* 
有一个新用户连接，acceptor会执行这个回调操作，负责将mainLoop接收到的请求连接(acceptChannel_会有读事件发生)通过回调轮询分发给subLoop去处理
职责：建立连接+轮询选择EventLoop实现负载均衡+设置回调函数
*/
void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr)
{
   // 负载均衡：轮询算法 选择一个subLoop 来管理connfd对应的channel
    EventLoop *ioLoop = threadPool_->getNextLoop();

    //命名
    char buf[64] = {0};
    snprintf(buf, sizeof(buf), "-%s#%d", ipPort_.c_str(), nextConnId_);
    ++nextConnId_;  // 这里没有设置为原子类是因为其只在mainloop中执行 不涉及线程安全问题
    std::string connName = name_ + buf;

    LOG_INFO<<"TcpServer::newConnection ["<<name_.c_str()<<"]- new connection ["<<connName.c_str()<<"]from "<<peerAddr.toIpPort().c_str();
    
    // 通过sockfd获取其绑定的本机的ip地址和端口信息(由本地服务器自动分配)
    sockaddr_in local;
    ::memset(&local, 0, sizeof(local));
    socklen_t addrlen = sizeof(local);
    if(::getsockname(sockfd, (sockaddr *)&local, &addrlen) < 0)
    {
        LOG_ERROR<<"sockets::getLocalAddr";
        //错误恢复 使用默认地址继续执行？
    }

    InetAddress localAddr(local);

    //创建TcpConnection对象管理新连接
    TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                            connName,
                                            sockfd,
                                            localAddr,
                                            peerAddr));
    //将新连接加入到连接映射表中(主线程安全)
    connections_[connName] = conn;

    // ▲下面的回调都是用户设置给TcpServer => TcpConnection的，至于Channel绑定的则是TcpConnection设置的四个，handleRead,handleWrite... 这下面的回调用于handlexxx函数中
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    // 设置了如何关闭连接的回调
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));

    //在子线程中执行连接建立逻辑
    ioLoop->runInLoop(
        std::bind(&TcpConnection::connectEstablished, conn));       //▲对比：不能使用.get()传递原始指针，如果该conn突然被其他线程销毁，则传递的原始指针将导致错误  //可以继续深入学习智能指针和生命周期管理
}

//连接关闭的回调(在TcpConnection::handleClose()处通过设置的回调调用)
void TcpServer::removeConnection(const TcpConnectionPtr &conn)
{
    //转移到主线程上执行(保证线程安全)
    loop_->runInLoop(
        std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

/*
▲很多方法都使用了xxxInLoop的线程安全方法 借助One Loop Per Thread原则和任务队列 实现高效的无竞争的无锁操作
在主线程中实际执行连接移除(1.从连接映射表中移除 2.在所属的loop中执行连接销毁)
*/
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn)
{
    LOG_INFO<<"TcpServer::removeConnectionInLoop ["<<
             name_.c_str()<<"] - connection %s"<<conn->name().c_str();

    connections_.erase(conn->name());       //从连接表中移除
    EventLoop *ioLoop = conn->getLoop();    //获取相应的loop
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn));     //在相应的loop的任务队列中关闭连接---使用queueInLoop是为了让前面的任务先执行完
}