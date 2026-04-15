#pragma once

#include "noncopyable.h"

class InetAddress;

/*
封装socket 既可用于lfd 也可用于cfd
职责：执行listen、bind、accept等底层系统调用 
只关心怎么执行 而不关心何时执行
*/
class Socket : noncopyable
{
public:
    explicit Socket(int sockfd)
        : sockfd_(sockfd)
    {
    }
    ~Socket();

    int fd() const { return sockfd_; }
    void bindAddress(const InetAddress &localaddr);     //封装bind
    void listen();                                      //封装listen
    int accept(InetAddress *peeraddr);                  //封装accept   peeraddr：(传出参数)客户端地址结构

    void shutdownWrite();                               //设置半关闭状态(关闭本端的写缓冲区 向对方发送FIN包 表示本端数据发送完毕 但是仍可接收数据)

    void setNonBlocking();                              //设置非阻塞Socket

    void setTcpNoDelay(bool on);                        //设置 TCP_NODELAY选项 允许小数据包的发送 减少延迟
    void setReuseAddr(bool on);                         //设置 SO_REUSEADDR选项 允许端口复用 允许服务器快速重启
    void setReusePort(bool on);                         //设置 SO_REUSEPORT选项 允许多个socket绑定到相同的ip和端口组合 -> 多线程中实现负载均衡
    void setKeepAlive(bool on);                         //设置 SO_KEEPALIVE选项 检测空闲连接的对端是否依然存活

private:
    const int sockfd_;      //初始化后不能修改
};
/*
封装的目的：不需要过度关心底层实现(参数 错误处理等)
*/
