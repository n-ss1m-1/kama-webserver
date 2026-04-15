
#include <Channel.h>
#include <EventLoop.h>
#include <Logger.h>

const int Channel::kNoneEvent = 0; //空事件
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI; //读事件   其中EPOLLPRI特别表示紧急读事件
const int Channel::kWriteEvent = EPOLLOUT; //写事件

// EventLoop: ChannelList Poller
Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop)
    , fd_(fd)
    , events_(0)  //设置为ET模式
    , revents_(0)
    , index_(-1)        //channel初始状态为：未添加到poller中
    , tied_(false)
{
}

Channel::~Channel()
{
}

// channel的tie方法什么时候调用过?  TcpConnection => channel
/**
 * TcpConnection中注册了Channel对应的回调函数，传入的回调函数均为TcpConnection
 * 对象的成员方法，因此可以说明一点就是：Channel的结束一定晚于TcpConnection对象！
 * 此处用tie去解决TcpConnection和Channel的生命周期时长问题，从而保证了Channel对象能够在
 * TcpConnection销毁前销毁。
 **/
void Channel::tie(const std::shared_ptr<void> &obj)
{
    tie_ = obj;
    tied_ = true;
}

//update 和remove => EpollPoller 更新channel在poller中的状态
/*
当改变channel所表示的fd的events事件后，update负责在poller内更改fd相应的监听事件(=epoll_ctl)
eventloop带有poller成员->封装poller的ctl方法 + channel带有eventloop成员->调用其封装的poller方法
*/
void Channel::update()
{
    // 通过channel所属的eventloop，调用poller的相应方法，注册fd的events事件
    loop_->updateChannel(this);
}

// 从Poller中移除Channel
void Channel::remove()
{
    loop_->removeChannel(this);
}

void Channel::handleEvent(Timestamp receiveTime)
{
    if (tied_)  //对于需要保护的对象(如TcpConnection)
    {
        std::shared_ptr<void> guard = tie_.lock();      //尝试将TcpConnection对象弱引用提升为强引用(保证在回调期间 该TcpConnection对象始终存活)
        if (guard)  //TcpConnection对象还存在
        {
            handleEventWithGuard(receiveTime);
        }
        // 如果提升失败了 就不做任何处理 说明Channel的TcpConnection对象已经不存在了
    }
    else    //对于生命周期稳定的对象(如acceptorSocket)
    {
        handleEventWithGuard(receiveTime);
    }
}

void Channel::handleEventWithGuard(Timestamp receiveTime)   //注意处理的顺序
{
    LOG_INFO<<"channel handleEvent revents:"<<revents_;
    // 关闭  只有当连接确定已挂起 && 没有剩余数据可读时(避免数据丢失) -> 才立即关闭连接
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) // 当TcpConnection对应Channel 通过shutdown 关闭写端 epoll触发EPOLLHUP(表示对端已关闭连接)
    {
        if (closeCallback_)
        {
            closeCallback_();
        }
    }
    // 错误
    if (revents_ & EPOLLERR)        //epoll自动监听
    {
        if (errorCallback_)
        {
            errorCallback_();
        }
    }
    // 读
    if (revents_ & (EPOLLIN | EPOLLPRI))
    {
        if (readCallback_)
        {
            readCallback_(receiveTime);
        }
    }
    // 写
    if (revents_ & EPOLLOUT)
    {
        if (writeCallback_)
        {
            writeCallback_();
        }
    }
}