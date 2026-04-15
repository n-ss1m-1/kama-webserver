#pragma once

/**
 * 用户使用muduo编写服务器程序
 **/

#include <functional>
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>

#include "EventLoop.h"
#include "Acceptor.h"
#include "InetAddress.h"
#include "noncopyable.h"
#include "EventLoopThreadPool.h"
#include "Callbacks.h"
#include "TcpConnection.h"
#include "Buffer.h"

/**
 * TcpServer - 网络服务器主类（用户接口类）
 * 
 * 设计模式：门面模式（Facade） + Reactor模式
 * 核心架构：主从多Reactor线程模型
 * 线程模型：One Loop Per Thread + 线程池
 */
class TcpServer : noncopyable
{
public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    enum Option
    {
        kNoReusePort,   //不允许重用本地端口
        kReusePort,     //允许重用本地端口
    };

    TcpServer(EventLoop *loop,
              const InetAddress &listenAddr,
              const std::string &nameArg,
              Option option = kReusePort);
    ~TcpServer();

    //回调函数设置接口
    void setThreadInitCallback(const ThreadInitCallback &cb) { threadInitCallback_ = cb; }      //未使用
    void setConnectionCallback(const ConnectionCallback &cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback &cb) { writeCompleteCallback_ = cb; }

    // 设置底层subloop的个数(必须在start前调用)
    void setThreadNum(int numThreads);

    /**
     * 启动服务器：
     * - 线程安全：可多次调用，只有第一次有效
     * - 幂等性：重复调用无副作用
     * - 异步启动：非阻塞，立即返回
     */
    void start();

private:
    void newConnection(int sockfd, const InetAddress &peerAddr);    //新连接到来的回调(由Acceptor调用)
    void removeConnection(const TcpConnectionPtr &conn);            //连接需要关闭的回调(由TcpConnection调用)
    void removeConnectionInLoop(const TcpConnectionPtr &conn);      //在主线程中实际执行连接移除

    using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;    //连接名称->TcpConnectionPtr

    EventLoop *loop_; // ▲baseloop 用户自定义的loop

    const std::string ipPort_;      //监听socket的地址的字符串表示
    const std::string name_;        //服务器名称(用于日志标识)

    std::unique_ptr<Acceptor> acceptor_; // ▲运行在mainloop 任务就是监听新连接事件

    std::shared_ptr<EventLoopThreadPool> threadPool_; //▲线程池 one loop per thread

    ConnectionCallback connectionCallback_;         // 连接需要 建立/关闭 时的回调？
    MessageCallback messageCallback_;               // 接受到客户数据后的回调操作---业务逻辑操作(此处为回响)
    WriteCompleteCallback writeCompleteCallback_;   // 消息发送完成后的回调(通知上层)
    ThreadInitCallback threadInitCallback_;         // loop线程初始化的回调     //未使用

    int numThreads_;            //线程池中线程的数量
    std::atomic_int started_;   //标识服务器启动状态
    int nextConnId_;            //连接ID生成器
    ConnectionMap connections_; // ▲保存所有的连接
};
/*
为什么既存在成员函数(如newConnection) 又使用function封装了回调成员变量(如connectionCallback_)：
- 其中newConnection是TcpServer类内自用的 
- 而回调成员函数是由上层调用set方法设置的 目的是为了在相应事件发生时 调用相应的回调函数 实现功能(通知 写入日志等)
--- 回调函数的调用位置规定了："什么时候做什么事"(框架职责) 
--- 而回调函数的set则为用户提供了极大灵活性(用户自定义该回调函数)："规定具体应该做什么事"(用户职责)
注：一般在上一层的类的构造函数中 对下一层的对象(组合 作为上层类的成员变量)进行设置回调函数
例：
1.
此处的connectionCallback_会在TcpConnection::connectEstablished()的末尾执行
2.
TcpServer::messageCallback_在TcpServer的构造函数中 设置为TcpConnection的回调函数
---然后在TcpConnection::handleRead()中调用
---而TcpConnection的构造函数中 又将TcpConnection::handleRead()设置为Channel的回调函数Channel::readCallback_
---Poller通过poll()方法传出activeChannels 在EventLoop::loop()中取出每个activeChannel 然后调用Channel::handleEvent()->Channel::handleEventWithGuard()
---在Channel::handleEventWithGuard()中判断发生的事件 再调用相应的channel的回调函数
3.
TcpServer::newConnection()中调用conn->setCloseCallback() 将其设置为TcpServer::removeConnection()
---实际需要关闭连接时(TcpConnection::handleClose()处) TcpConnection回调该函数
4.
当有新用户连接时，Acceptor类中绑定的acceptChannel_会有读事件发生，执行Acceptor::handleRead() 
---在其内调用TcpServer设置的回调函数Acceptor::NewConnectionCallback_
5.
此处没有使用TcpConnection::setHighWaterMarkCallback()
*/