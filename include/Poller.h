#pragma once

#include <vector>
#include <unordered_map>

#include "noncopyable.h"
#include "Timestamp.h"

class Channel;
class EventLoop;

// muduo库中多路事件分发器的核心IO复用模块
class Poller
{
public:
    using ChannelList = std::vector<Channel *>;

    Poller(EventLoop *loop);
    virtual ~Poller() = default;

    // 给所有IO复用保留统一的接口
    virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;     //▲ ≈ epoll_wait() activeChannels为传出参数 返回有事件发生的channel列表
    virtual void updateChannel(Channel *channel) = 0;        // ≈ epoll_ctl(MOD或ADD)
    virtual void removeChannel(Channel *channel) = 0;        // ≈ epoll_ctl(DEL)

    // 判断参数channel是否在当前的Poller当中
    bool hasChannel(Channel *channel) const;

    /*
    EventLoop可以通过该接口获取默认的IO复用的具体实现?：
    newDefaultPoller()是一个工厂方法，
    它根据当前操作系统自动选择最合适的I/O多路复用实现（Linux用epoll，macOS用kqueue），
    让EventLoop无需关心底层平台差异。
    */
    static Poller *newDefaultPoller(EventLoop *loop);

protected:
    // (cfd->channel) map的key:sockfd value:sockfd所属的channel通道类型
    using ChannelMap = std::unordered_map<int, Channel *>;
    ChannelMap channels_;

private:
    EventLoop *ownerLoop_; // 定义Poller所属的事件循环EventLoop
};

/*
使用Poller的好处：
1.平台兼容、代码复用：可以通过Poller抽象接口支持多平台(epoll、kqueue)
2.不需要关注底层细节：众多参数和结构体
3.职责分离、架构清晰
*/