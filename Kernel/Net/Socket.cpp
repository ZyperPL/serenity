/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/Net/IPv4Socket.h>
#include <Kernel/Net/LocalSocket.h>
#include <Kernel/Net/Socket.h>
#include <Kernel/Process.h>
#include <Kernel/UnixTypes.h>
#include <LibC/errno_numbers.h>

//#define SOCKET_DEBUG

namespace Kernel {

KResultOr<NonnullRefPtr<Socket>> Socket::create(int domain, int type, int protocol)
{
    switch (domain) {
    case AF_LOCAL:
        return LocalSocket::create(type & SOCK_TYPE_MASK);
    case AF_INET:
        return IPv4Socket::create(type & SOCK_TYPE_MASK, protocol);
    default:
        return KResult(-EAFNOSUPPORT);
    }
}

Socket::Socket(int domain, int type, int protocol)
    : m_domain(domain)
    , m_type(type)
    , m_protocol(protocol)
{
    auto& process = *Process::current();
    m_origin = { process.pid().value(), process.uid(), process.gid() };
}

Socket::~Socket()
{
}

void Socket::set_setup_state(SetupState new_setup_state)
{
#ifdef SOCKET_DEBUG
    dbg() << "Socket{" << this << "} setup state moving from " << to_string(m_setup_state) << " to " << to_string(new_setup_state);
#endif

    m_setup_state = new_setup_state;
}

RefPtr<Socket> Socket::accept()
{
    LOCKER(m_lock);
    if (m_pending.is_empty())
        return nullptr;
#ifdef SOCKET_DEBUG
    dbg() << "Socket{" << this << "} de-queueing connection";
#endif
    auto client = m_pending.take_first();
    ASSERT(!client->is_connected());
    auto& process = *Process::current();
    client->m_acceptor = { process.pid().value(), process.uid(), process.gid() };
    client->m_connected = true;
    client->m_role = Role::Accepted;
    return client;
}

KResult Socket::queue_connection_from(NonnullRefPtr<Socket> peer)
{
#ifdef SOCKET_DEBUG
    dbg() << "Socket{" << this << "} queueing connection";
#endif
    LOCKER(m_lock);
    if (m_pending.size() >= m_backlog)
        return KResult(-ECONNREFUSED);
    m_pending.append(peer);
    return KSuccess;
}

KResult Socket::setsockopt(int level, int option, Userspace<const void*> user_value, socklen_t user_value_size)
{
    ASSERT(level == SOL_SOCKET);
    switch (option) {
    case SO_SNDTIMEO:
        if (user_value_size != sizeof(timeval))
            return KResult(-EINVAL);
        copy_from_user(&m_send_timeout, static_ptr_cast<const timeval*>(user_value));
        return KSuccess;
    case SO_RCVTIMEO:
        if (user_value_size != sizeof(timeval))
            return KResult(-EINVAL);
        copy_from_user(&m_receive_timeout, static_ptr_cast<const timeval*>(user_value));
        return KSuccess;
    case SO_BINDTODEVICE: {
        if (user_value_size != IFNAMSIZ)
            return KResult(-EINVAL);
        auto user_string = static_ptr_cast<const char*>(user_value);
        auto ifname = Process::current()->validate_and_copy_string_from_user(user_string, user_value_size);
        if (ifname.is_null())
            return KResult(-EFAULT);
        auto device = NetworkAdapter::lookup_by_name(ifname);
        if (!device)
            return KResult(-ENODEV);
        m_bound_interface = device;
        return KSuccess;
    }
    case SO_KEEPALIVE:
        // FIXME: Obviously, this is not a real keepalive.
        return KSuccess;
    default:
        dbg() << "setsockopt(" << option << ") at SOL_SOCKET not implemented.";
        return KResult(-ENOPROTOOPT);
    }
}

KResult Socket::getsockopt(FileDescription&, int level, int option, Userspace<void*> value, Userspace<socklen_t*> value_size)
{
    socklen_t size;
    if (!Process::current()->validate_read_and_copy_typed(&size, value_size))
        return KResult(-EFAULT);

    ASSERT(level == SOL_SOCKET);
    switch (option) {
    case SO_SNDTIMEO:
        if (size < sizeof(timeval))
            return KResult(-EINVAL);
        copy_to_user(static_ptr_cast<timeval*>(value), &m_send_timeout);
        size = sizeof(timeval);
        copy_to_user(value_size, &size);
        return KSuccess;
    case SO_RCVTIMEO:
        if (size < sizeof(timeval))
            return KResult(-EINVAL);
        copy_to_user(static_ptr_cast<timeval*>(value), &m_receive_timeout);
        size = sizeof(timeval);
        copy_to_user(value_size, &size);
        return KSuccess;
    case SO_ERROR: {
        if (size < sizeof(int))
            return KResult(-EINVAL);
        dbg() << "getsockopt(SO_ERROR): FIXME!";
        int errno = 0;
        copy_to_user(static_ptr_cast<int*>(value), &errno);
        size = sizeof(int);
        copy_to_user(value_size, &size);
        return KSuccess;
    }
    case SO_BINDTODEVICE:
        if (size < IFNAMSIZ)
            return KResult(-EINVAL);
        if (m_bound_interface) {
            const auto& name = m_bound_interface->name();
            auto length = name.length() + 1;
            copy_to_user(static_ptr_cast<char*>(value), name.characters(), length);
            size = length;
            copy_to_user(value_size, &size);
            return KSuccess;
        } else {
            size = 0;
            copy_to_user(value_size, &size);

            return KResult(-EFAULT);
        }
    default:
        dbg() << "getsockopt(" << option << ") at SOL_SOCKET not implemented.";
        return KResult(-ENOPROTOOPT);
    }
}

KResultOr<size_t> Socket::read(FileDescription& description, size_t, u8* buffer, size_t size)
{
    if (is_shut_down_for_reading())
        return 0;
    return recvfrom(description, buffer, size, 0, {}, 0);
}

KResultOr<size_t> Socket::write(FileDescription& description, size_t, const u8* data, size_t size)
{
    if (is_shut_down_for_writing())
        return -EPIPE;
    return sendto(description, data, size, 0, {}, 0);
}

KResult Socket::shutdown(int how)
{
    if (type() == SOCK_STREAM && !is_connected())
        return KResult(-ENOTCONN);
    if (m_role == Role::Listener)
        return KResult(-ENOTCONN);
    if (!m_shut_down_for_writing && (how & SHUT_WR))
        shut_down_for_writing();
    if (!m_shut_down_for_reading && (how & SHUT_RD))
        shut_down_for_reading();
    m_shut_down_for_reading |= (how & SHUT_RD) != 0;
    m_shut_down_for_writing |= (how & SHUT_WR) != 0;
    return KSuccess;
}

}
