#pragma once

#include <sys/epoll.h>
#include <functional>
#include <memory>

#include "noncopyable.h"
#include "Timestamp.h"

class EventLoop;

/**
 * 理清楚 EventLoop、Channel、Poller之间的关系  Reactor模型上对应多路事件分发器
 * Channel理解为通道 封装了sockfd和其感兴趣的event 如EPOLLIN、EPOLLOUT事件 还绑定了poller返回的具体事件
 * 职责：只负责事件分发 但是不拥有管理的fd(属于相应TcpConnection)
 **/
class Channel : noncopyable
{
public:
    using EventCallback = std::function<void()>; // muduo仍使用typedef
    using ReadEventCallback = std::function<void(Timestamp)>;

    Channel(EventLoop *loop, int fd);
    ~Channel();

    /*
    fd得到Poller通知以后 处理事件 handleEvent在EventLoop::loop()中调用
    通过handleEvent判断安全性后 再进行实际处理(handleEventWithGuard)
    */
    void handleEvent(Timestamp receiveTime);

    // 在TcpConnection处调用 设置回调函数对象
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    // 建立当前Channel和相应TcpConnection的绑定
    void tie(const std::shared_ptr<void> &);

    int fd() const { return fd_; }
    int events() const { return events_; }
    void set_revents(int revt) { revents_ = revt; }

    // 设置fd相应的监听事件 相当于epoll_ctl add mod
    void enableET() {events_ |= EPOLLET;}
    void enableReading() { events_ |= kReadEvent; update(); }       //设置监听EPOLLIN    update挂树
    void disableReading() { events_ &= ~kReadEvent; update(); }     //设置不监听EPOLLIN  update挂树
    void enableWriting() { events_ |= kWriteEvent; update(); }      //设置监听EPOLLOUT   update挂树
    void disableWriting() { events_ &= ~kWriteEvent; update(); }    //设置不监听EPOLLOUT update挂树
    void disableAll() { events_ = kNoneEvent; update(); }           //设置不监听任何事件  update挂树

    // 返回fd当前监听的事件
    bool isNoneEvent() const { return events_ == kNoneEvent; }  //是否没有监听事件
    bool isWriting() const { return events_ & kWriteEvent; }    //是否在监听可写事件
    bool isReading() const { return events_ & kReadEvent; }     //是否在监听可读事件

    // 返回当前channel在Poller中的状态
    int index() { return index_; }
    void set_index(int idx) { index_ = idx; }

    // one loop per thread
    EventLoop *ownerLoop() { return loop_; }

    // 从Poller中移除Channel(从树上摘除 epoll_ctl的DEL)
    void remove();
private:

    void update();
    void handleEventWithGuard(Timestamp receiveTime);   //实际的事件处理

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop *loop_; // 事件循环->可以使用封装的poller方法了
    const int fd_;    // fd，Poller监听的对象  一个channel对应管理一个fd
    int events_;      // 注册fd感兴趣的事件
    int revents_;     // Poller返回的具体发生的事件
    int index_;       //用于标识区分channel在Poller中的状态

    std::weak_ptr<void> tie_;   //弱引用绑定 指向TcpConnection 延长其生命周期
    bool tied_;                 //绑定标志(表示是否需要使用tie_保护其生命周期)

    // 因为channel通道里可获知fd最终发生的具体的事件events，所以它负责调用具体事件的回调操作
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};