#pragma once

#include <functional>

#include "noncopyable.h"
#include "Socket.h"
#include "Channel.h"

class EventLoop;
class InetAddress;

/*
职责：设置监听状态、接受新连接、回调上层设置的函数(newconnection_cb)
关心何时进行操作(业务逻辑) 而不关心如何操作(与socket职责分明)
组合优于继承：Acceptor拥有Socket
*/
class Acceptor : noncopyable
{
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress &)>;

    Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport);
    ~Acceptor();
    //设置有新连接的回调函数
    void setNewConnectionCallback(const NewConnectionCallback &cb) { NewConnectionCallback_ = cb; }
    // 判断是否在监听
    bool listenning() const { return listenning_; }
    // 启动监听
    void listen();

private:
    void handleRead();          //(事件处理核心)处理新用户的连接事件

    EventLoop *loop_;           // Acceptor用的就是用户定义的那个baseLoop 也称作mainLoop
    Socket acceptSocket_;       //专门用于接收新连接的socket
    Channel acceptChannel_;     //专门用于监听新连接的channel
    NewConnectionCallback NewConnectionCallback_;       //成功接受新连接之后 调用该回调函数 //这个回调函数通常由TcpServer提供，负责将这个新的连接socket分发给某个子Reactor（subLoop）进行处理
    bool listenning_;           //是否在监听
};