#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <Acceptor.h>
#include <Logger.h>
#include <InetAddress.h>

//创建一个非阻塞监听socket
static int createNonblocking()
{
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0)
    {
        LOG_FATAL << "listen socket create err " << errno;
    }
    return sockfd;
}

Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : loop_(loop)
    , acceptSocket_(createNonblocking())
    , acceptChannel_(loop, acceptSocket_.fd())
    , listenning_(false)
{
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(true);
    acceptSocket_.bindAddress(listenAddr);
    // TcpServer::start() => Acceptor.listen() 如果有新用户连接 要执行一个回调(accept => connfd => 打包成Channel => 唤醒subloop)
    // baseloop监听到有事件发生 => acceptChannel_(listenfd) => 执行该回调函数
    acceptChannel_.setReadCallback(
        std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor()   //acceptSocket_析构时会关闭sockFd_
{
    acceptChannel_.disableAll();    // 把从Poller中感兴趣的事件删除掉(events=NoneEvent+从树上摘下) + index_=Deleted
    acceptChannel_.remove();        // 调用EventLoop->removeChannel => Poller->removeChannel 把Poller的ChannelMap对应的部分删除 + index_=New
    /*▲第二步的功能可以覆盖第一步 但是这里是防御型编程 第一步-停职 第二步-开除*/
}

//▲启动监听(!=epoll_wait)
void Acceptor::listen()
{
    listenning_ = true;
    acceptSocket_.listen();         // 普通的listen方法
    acceptChannel_.enableReading(); // 将监听acceptChannel_的 读事件 ▲注册至Poller(等待连接) !重要
    acceptChannel_.enableET();
}

// listenfd有事件发生了，就是有新用户连接了
void Acceptor::handleRead()
{
    while(true)
    {
        InetAddress peerAddr;
        int connfd = acceptSocket_.accept(&peerAddr);       //返回新连接的sockfd
        int savedErrno = errno;
        if (connfd >= 0)
        {
            if (NewConnectionCallback_) //若TcpServer设置了回调 则执行
            {
                NewConnectionCallback_(connfd, peerAddr); // 轮询找到subLoop 唤醒并分发当前的新客户端的Channel
            }
            else    //若未设置回调 则关闭连接(避免无人管理且占用资源)
            {
                ::close(connfd);
            }
        }
        else
        {
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)
            {
                // ET模式核心：没有新连接了，退出循环
                break;
            }
            LOG_ERROR<<"accept Err";
            if (savedErrno == EMFILE)
            {
                LOG_ERROR<<"sockfd reached limit";
            }
            break;
        }
    }

}