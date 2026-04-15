#pragma once

#include <memory>
#include <string>
#include <atomic>

#include "noncopyable.h"
#include "InetAddress.h"
#include "Callbacks.h"
#include "Buffer.h"
#include "Timestamp.h"

class Channel;
class EventLoop;
class Socket;
/*
与其他类的关系：
1.一个TcpConnection属于一个eventloop(多对一)
2.TcpConnection组合了Socket、Channel、Buffer
3.所有TcpConnection受到TcpServer管理(设置状态和回调等)
*/

/**
 * TcpServer => Acceptor => 有一个新用户连接，通过accept函数拿到connfd
 * => TcpConnection设置回调 => 设置到Channel => Poller监听 => Channel回调
 **/

class TcpConnection : private noncopyable, public std::enable_shared_from_this<TcpConnection>
{
public:
    TcpConnection(EventLoop *loop,
                  const std::string &nameArg,
                  int sockfd,
                  const InetAddress &localAddr,
                  const InetAddress &peerAddr);
    ~TcpConnection();

    // 连接建立
    void connectEstablished();
    // 连接销毁
    void connectDestroyed();

    // 发送数据
    void send(const std::string &buf);      //发送字符串
    void sendFile(int fileDescriptor, off_t offset, size_t count);  //大文件 零拷贝
    
    // 进入kDisconnecting状态 根据数据是否发送完毕->是否关闭写端 进入半关闭状态 // 然后等待对方发送数据完毕之后,调用closeCallback_彻底关闭连接
    void shutdown();

    /*
    线程安全的内部实现方法(在loop所属线程中执行)
    send和sendInLoop的区别：
    1.send是可供外部使用的方法, 在其内部检查是否是线程安全的(isInLoopThread()), 然后才调用sendInLoop,执行任务/否则将sendInLoop设置为回调，加入该loop的任务队列中,使用eventfd唤醒所属线程,执行任务
    2.sendInLoop是内部的方法,只关注send的功能实现,默认已在线程安全的环境中
    */
    void sendInLoop(const void *data, size_t len);
    void sendFileInLoop(int fileDescriptor, off_t offset, size_t count);
    void shutdownInLoop();          //继续关闭流程(调用Socket的shutdownwrite() 发送FIN 真正实现半关闭)

    //Setter:
    //设置TcpConnection的回调函数
    void setConnectionCallback(const ConnectionCallback &cb)
    { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb)
    { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback &cb)
    { writeCompleteCallback_ = cb; }
    void setCloseCallback(const CloseCallback &cb)
    { closeCallback_ = cb; }
    void setHighWaterMarkCallback(const HighWaterMarkCallback &cb, size_t highWaterMark)
    { highWaterMarkCallback_ = cb; highWaterMark_ = highWaterMark; }

    //Getter:
    //判断是否已连接
    bool connected() const { return state_ == kConnected; } 

    EventLoop *getLoop() const { return loop_; }
    const std::string &name() const { return name_; }
    const InetAddress &localAddress() const { return localAddr_; }
    const InetAddress &peerAddress() const { return peerAddr_; }

private:
    enum StateE
    {
        kDisconnected, // 已经断开连接
        kConnecting,   // 正在连接
        kConnected,    // 已连接
        kDisconnecting // 正在断开连接(半关闭)
    };
    void setState(StateE state) { state_ = state; }

    //Channel事件回调处理函数
    void handleRead(Timestamp receiveTime);
    void handleWrite();
    void handleClose(); // 连接关闭处理：收到对端FIN包或主动关闭
    void handleError();


    EventLoop *loop_;           // 这里是baseloop还是subloop由TcpServer中创建的线程数决定 若为多Reactor 该loop_指向subloop 若为单Reactor 该loop_指向baseloop
    const std::string name_;    //连接名称 用于日志和调试
    std::atomic_int state_;     //连接状态(原子操作 保证线程安全)
    bool reading_;              //连接是否在监听读事件

    // Socket Channel 这里和Acceptor类似    Acceptor => mainloop    TcpConnection => subloop
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

    const InetAddress localAddr_;       //本地地址
    const InetAddress peerAddr_;        //对端地址

    // 这些回调TcpServer也有 用户通过写入TcpServer注册 TcpServer再将注册的回调传递给TcpConnection TcpConnection再将回调注册到Channel中
    ConnectionCallback connectionCallback_;         // 连接状态变化回调(通知上层应用)
    MessageCallback messageCallback_;               // 接受到客户数据后的回调操作---业务逻辑操作(此处为回响)
    WriteCompleteCallback writeCompleteCallback_;   // 消息发送完成以后的回调(通知上层)
    HighWaterMarkCallback highWaterMarkCallback_;   // 高水位回调
    CloseCallback closeCallback_;                   // 关闭连接的回调 执行的是TcpServer::removeConnection回调方法
    size_t highWaterMark_;                          // 高水位阈值

    // 数据缓冲区
    Buffer inputBuffer_;    // 服务器接收数据的缓冲区(等待EPOLLIN)
    Buffer outputBuffer_;   // 服务器发送数据的缓冲区(等待EPOLLOUT) 用户send向outputBuffer_发?
};

/*
服务器初始化网络连接模块的流程：
1.创建TcpServer和base loop(已封装成EchoServer类 构造时给TcpServer设置回调(连接状态变化、读写事件回调)) -> 启动
2.TcpServer的构造函数中：创建(new)acceptor和threadPool->设置回调 TcpServer启动中：threadpool启动 acceptor启动监听(加入任务队列？why) 
服务器接收连接的流程：

服务器发送数据流程：
先sendInLoop 再handleWrite？？?

服务器接收数据流程：

服务器主动关闭的流程：

服务器被动关闭连接的流程：

*/

