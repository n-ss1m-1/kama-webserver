#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>

/*
封装socket地址类型
职责：对相应socket的地址结构进行操作
(1.设置/获取地址结构 2.转换ip和port的字节序)
*/
class InetAddress
{
public:
    //lfd使用
    explicit InetAddress(uint16_t port = 0, std::string ip = "127.0.0.1");      //explicit:禁止传入参数的隐式转换(单参数的构造函数)
    //cfd使用
    explicit InetAddress(const sockaddr_in &addr)
        : addr_(addr)
    {
    }

    std::string toIp() const;       //转换为本地字节序ip
    std::string toIpPort() const;   //本地字节序 IP:端口
    uint16_t toPort() const;        //本地字节序 端口

    const sockaddr_in *getSockAddr() const { return &addr_; }
    void setSockAddr(const sockaddr_in &addr) { addr_ = addr; }     //设置地址结构

private:
    sockaddr_in addr_;
};