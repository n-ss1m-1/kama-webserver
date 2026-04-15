#include <functional>
#include <string>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <fcntl.h> // for open
#include <unistd.h> // for close

#include <TcpConnection.h>
#include <Logger.h>
#include <Socket.h>
#include <Channel.h>
#include <EventLoop.h>

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL<<" mainLoop is null!";
    }
    return loop;
}

TcpConnection::TcpConnection(EventLoop *loop,
                             const std::string &nameArg,
                             int sockfd,
                             const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(CheckLoopNotNull(loop))
    , name_(nameArg)
    , state_(kConnecting)
    , reading_(true)
    , socket_(new Socket(sockfd))
    , channel_(new Channel(loop, sockfd))
    , localAddr_(localAddr)
    , peerAddr_(peerAddr)
    , highWaterMark_(64 * 1024 * 1024) // 64M
{


    // 下面给channel设置相应的回调函数 poller给channel通知感兴趣的事件发生了 channel会回调相应的回调函数
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this));

    LOG_INFO<<"TcpConnection::ctor:["<<name_.c_str()<<"]at fd="<<sockfd;
    // 设置非阻塞socket
    socket_->setNonBlocking();
    socket_->setKeepAlive(true);        //持久连接
    socket_->setTcpNoDelay(true);       //禁用nagle算法
}

TcpConnection::~TcpConnection()
{
    LOG_INFO<<"TcpConnection::dtor["<<name_.c_str()<<"]at fd="<<channel_->fd()<<"state="<<(int)state_;
}

// 连接建立(下列操作不能在TcpConnection的构造函数中执行：1.阶段区分  2.shared_from_this()需要该对象构造完成才能使用)
void TcpConnection::connectEstablished()
{
    setState(kConnected);
    channel_->tie(shared_from_this());  // 重要：让Channel持有TcpConnection的weak_ptr，防止回调时TcpConnection对象被销毁(回调时在临时作用域提升为shared_ptr)
    channel_->enableET();               // 设置为ET模式
    channel_->enableReading();          // 向poller注册channel的EPOLLIN读事件

    // 新连接建立 执行回调(通知上层应用：连接已建立)
    connectionCallback_(shared_from_this());
}
// 连接销毁()
void TcpConnection::connectDestroyed()
{
    if (state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll(); // 把channel的所有感兴趣的事件从poller中删除掉
        connectionCallback_(shared_from_this());
    }
    channel_->remove(); // 把channel从poller中删除掉
}


/**
 * 可读事件处理(设置为Channel的回调)：当服务器读端有数据到达时触发
 * 这是数据接收的入口点，处理TCP流的粘包/拆包问题???
 */
void TcpConnection::handleRead(Timestamp receiveTime)
{
    int savedErrno = 0;
    while(true)
    {
        ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
        if (n > 0) // 有数据到达
        {
            /*
            接收到客户发送的数据 调用上层应用传入(main->TcpServer->TcpConnection)的回调操作onMessage(对客户发送过来的数据进行业务逻辑处理--此处为回响) 
            shared_from_this()保证在处理期间TcpConnection对象不会被销毁
            */
            messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
        }
        else if (n == 0) // 客户端断开
        {
            handleClose();      //！！！此时服务器被动关闭连接
            break;
        }
        else if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) 
        {
            // 关键：ET 模式下，读到 EAGAIN 说明数据已读完
            break;
        }
        else // 出错了
        {
            errno = savedErrno;
            LOG_ERROR<<"TcpConnection::handleRead";
            handleError();
            break;
        }
    }
}

/**
 * 可写事件处理(设置为Channel的回调)：当内核发送缓冲区有空闲空间时触发
 * 继续发送outputBuffer_中缓冲的数据
 */
void TcpConnection::handleWrite()
{
    if (channel_->isWriting())      //防御性编程
    {
        int savedErrno = 0;
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
        if (n > 0)
        {
            outputBuffer_.retrieve(n);  //回收已读数据空间：移动读指针
            //检查发送缓冲区是否清空
            if (outputBuffer_.readableBytes() == 0)
            {
                channel_->disableWriting(); //！注销监听的可写事件
                if (writeCompleteCallback_) //通知应用层：数据发送完毕
                {
                    // TcpConnection对象在其所在的subloop中 向pendingFunctors_中加入回调
                    loop_->queueInLoop(
                        std::bind(writeCompleteCallback_, shared_from_this()));         //▲该回调函数为成员变量 不需要取地址！
                }
                /*
                连接正在断开(之前已执行shutdown 但是发送缓冲区中还有数据未发送 发送完毕后 在handlewrite此处调用真正的关闭)
                ->继续关闭流程(调用Socket的shutdownwrite() 发送FIN 真正实现半关闭)
                */
                if (state_ == kDisconnecting)
                {
                    shutdownInLoop();
                }
            }
        }
        else
        {
            LOG_ERROR<<"TcpConnection::handleWrite";
        }
    }
    else
    {
        LOG_ERROR<<"TcpConnection fd="<<channel_->fd()<<"is down, no more writing";
    }
}

// 连接关闭处理：收到对端FIN包或主动关闭
void TcpConnection::handleClose()
{
    LOG_INFO<<"TcpConnection::handleClose fd="<<channel_->fd()<<"state="<<(int)state_;
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr connPtr(shared_from_this());   //在 handleClose函数的整个执行期间，无论外部其他引用如何变化，这个对象都绝对不会被销毁。->保证安全执行回调
    connectionCallback_(connPtr); // 连接状态变化 回调通知应用层
    closeCallback_(connPtr);      // 执行关闭连接的回调 执行的是TcpServer::removeConnection回调方法   // must be the last line
}       //这两步不加入任务队列的原因：避免

void TcpConnection::handleError()
{
    int optval;
    socklen_t optlen = sizeof(optval);
    int err = 0;

    // 获取socket 更精确的错误码 (getsockopt())
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err = errno;    //getsockopt()返回值<0 表示查询失败 查询失败原因保存在errno中
    }
    else    //getsockopt()返回值>=0 表示查询成功 错误值保存在optval中
    {
        err = optval;
    }
    LOG_ERROR<<"TcpConnection::handleError name:"<<name_.c_str()<<"- SO_ERROR:%"<<err;

    handleClose();
}


void TcpConnection::send(const std::string &buf)
{
    if (state_ == kConnected)    //连接状态才发送
    {
        if (loop_->isInLoopThread()) // 这种是对于单个reactor的情况 用户调用conn->send时 loop_即为当前线程
        {
            sendInLoop(buf.c_str(), buf.size());
        }
        else    //将任务(设置为回调)添加到当前loop的任务队列+唤醒相应线程 执行任务
        {
            loop_->runInLoop(
                std::bind(&TcpConnection::sendInLoop, this, buf.c_str(), buf.size()));  //通过提前绑定参数 形成一个不需要传参的可调用对象->符合functor形式
        }
    }
}

/**
 * 核心发送逻辑：在IO线程中执行
 * 采用智能发送策略：先尝试直接发送，失败则缓冲+事件监听
 * 解决：应用层发送快 vs 内核发送慢的速度不匹配问题
 */
void TcpConnection::sendInLoop(const void *data, size_t len)
{
    ssize_t nwrote = 0;
    size_t remaining = len;     
    bool faultError = false;

    if (state_ == kDisconnected) // 之前调用过该connection的shutdown 不能再进行发送了
    {
        LOG_ERROR<<"disconnected, give up writing";
    }

    // 表示channel_第一次开始写数据(且缓冲区没有待发送数据->保证应用层发送数据的顺序性)->尝试直接发送 (因为直接write无需经过发送缓冲区 最高效)
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0)
        {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_)   //回调通知
            {
                // 既然在这里数据全部发送完成，就不用再给channel设置epollout事件了
                loop_->queueInLoop(
                    std::bind(writeCompleteCallback_, shared_from_this()));     //▲该回调函数为成员变量 不需要取地址！
            }
        }
        else // nwrote < 0
        {
            nwrote = 0;
            if (errno != EWOULDBLOCK) // EWOULDBLOCK表示非阻塞情况下内核缓冲区已满后的正常返回 等同于EAGAIN (非阻塞模式在socket处设置)
            {
                LOG_ERROR<<"TcpConnection::sendInLoop";
                if (errno == EPIPE || errno == ECONNRESET) // SIGPIPE RESET
                {
                    faultError = true;
                }
            }
        }
    }
    /**
     * 如果还有剩余数据需要发送（可能是部分发送或完全未发送）
     * 将剩余数据存入发送缓冲区，并开始监听可写事件(EPOLLOUT)
     * (可写后调用:)channel的writeCallback_ 实际上就是TcpConnection设置的handleWrite回调
     **/
    //remaining: 第一次尝试发送后剩余的数据量
    //oldLen: 发送缓冲区中尚未发送的历史数据量
    if (!faultError && remaining > 0)
    {
        // 高水位检查：防止发送缓冲区无限增长
        size_t oldLen = outputBuffer_.readableBytes();      //oldLen:之前剩余未发送 暂存在缓冲区的数据    
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ && highWaterMarkCallback_)
        {
            loop_->queueInLoop(
                std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));     //▲该回调函数为成员变量 不需要取地址！
        }

        outputBuffer_.append((char *)data + nwrote, remaining);     //将剩余数据拷贝到发送缓冲区中 等待可写(EPOLLOUT)事件发生 才继续向cfd中写数据
        if (!channel_->isWriting())
        {
            channel_->enableWriting(); // 这里一定要注册channel的写事件 否则poller不会给channel通知epollout
        }
    }
}

// 进入kDisconnecting状态 根据数据是否发送完毕->是否关闭写端 进入半关闭状态 // 然后等待对方发送数据完毕之后,调用closeCallback_彻底关闭连接
void TcpConnection::shutdown()
{
    if (state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->runInLoop(
            std::bind(&TcpConnection::shutdownInLoop, this));
    }
}

void TcpConnection::shutdownInLoop()
{
    if (!channel_->isWriting()) // 说明当前outputBuffer_的数据全部向外发送完成 | 未发送完毕之前不能关闭写端
    {
        socket_->shutdownWrite();
    }
}





// 新增的零拷贝发送函数(考虑是否线程安全)
void TcpConnection::sendFile(int fileDescriptor, off_t offset, size_t count) {
    if (connected()) {
        if (loop_->isInLoopThread()) { // 判断当前线程是否是loop循环的线程
            sendFileInLoop(fileDescriptor, offset, count);
        }else{ // 如果不是，则唤醒运行这个TcpConnection的线程执行Loop循环
            loop_->runInLoop(
                std::bind(&TcpConnection::sendFileInLoop, shared_from_this(), fileDescriptor, offset, count));
        }
    } else {
        LOG_ERROR<<"TcpConnection::sendFile - not connected";
    }
}

/**
 * 零拷贝文件发送实现：使用sendfile系统调用
 * 优势：数据直接在内核空间传输，避免用户空间拷贝，大幅提升大文件传输性能
 * 注：同一大文件的多次排队发送会实现原子发送 不会在中间插入其他文件的任务(其他任务始终排在后面) (怎么实现的？)
 */
void TcpConnection::sendFileInLoop(int fileDescriptor, off_t offset, size_t count) {
    ssize_t bytesSent = 0;      // 发送了多少字节数
    size_t remaining = count;   // 还要多少数据要发送
    bool faultError = false;    // 错误的标志位

    //安全检查：若连接断开 则不发生数据
    if (state_ == kDisconnecting) { 
        LOG_ERROR<<"disconnected, give up writing";
        return;
    }

    // 表示Channel第一次开始写数据(且outputBuffer缓冲区中没有数据->保证应用层发送数据的顺序性)：尝试直接发送,不经过发送缓冲区
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        bytesSent = sendfile(socket_->fd(), fileDescriptor, &offset, remaining);    //sendfile：文件数据直接发送到网络 无需经过用户空间拷贝(offset传入传出参数 自动进行偏移更正)
        if (bytesSent >= 0) {
            remaining -= bytesSent;
            if (remaining == 0 && writeCompleteCallback_) {
                // remaining为0意味着数据正好全部发送完，就不需要给其设置写事件的监听。
                loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));      //▲该回调函数为成员变量 不需要取地址！
            }
        } else { // bytesSent < 0
            if (errno != EWOULDBLOCK) { // 如果是非阻塞没有数据返回错误这个是正常显现等同于EAGAIN，否则就异常情况
                LOG_ERROR<<"TcpConnection::sendFileInLoop";
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                faultError = true;
            }
        }
    }
    // 处理剩余数据(！与sendInLoop不同：不使用发送缓冲区 而是在任务队列中循环排队 重复使用sendfile进行零拷贝发送)
    if (!faultError && remaining > 0) {
        // 继续发送剩余数据
        loop_->queueInLoop(
            std::bind(&TcpConnection::sendFileInLoop, shared_from_this(), fileDescriptor, offset, remaining));  //offset传入传出参数 自动进行偏移更正
    }
}


/*▲
使用bind将回调函数添加到任务队列中：
1.作用：绑定对象和参数,生成可调用对象(function<void>型)
2.若可调用对象是: 成员变量(回调函数)->不需要&                                                     成员函数->有没有&都可以
3.若可调用对象是: 成员变量(回调函数)->不需要类的作用域说明(成员变量是属于对象的 一个对象有一份)      成员函数->需要类的作用域说明(成员函数是属于类的 一类只有一份)

*/