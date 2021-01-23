/*
    Copyright (c) 2007-2010 iMatix Corporation

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mailbox.hpp"
#include "platform.hpp"
#include "err.hpp"
#include "fd.hpp"
#include "ip.hpp"

#if defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif

zmq::fd_t zmq::mailbox_t::get_fd()
{
    return r;
}

zmq::mailbox_t::mailbox_t()
{
#ifdef PIPE_BUF
    //  Make sure that command can be written to the socket in atomic fashion.
    //  If this wasn't guaranteed, commands from different threads would be
    //  interleaved.
    zmq_assert(sizeof(command_t) <= PIPE_BUF);
#endif

    //  Create the socketpair for signaling.
    int rc = make_socketpair(&r, &w);
    errno_assert(rc == 0);

    //  Set the writer to non-blocking mode.
    int flags = fcntl(w, F_GETFL, 0);
    errno_assert(flags >= 0);
    rc = fcntl(w, F_SETFL, flags | O_NONBLOCK);
    errno_assert(rc == 0);

#ifndef MSG_DONTWAIT
    //  Set the reader to non-blocking mode.
    flags = fcntl(r, F_GETFL, 0);
    errno_assert(flags >= 0);
    rc = fcntl(r, F_SETFL, flags | O_NONBLOCK);
    errno_assert(rc == 0);
#endif
}

zmq::mailbox_t::~mailbox_t()
{
    close(w);
    close(r);
}

void zmq::mailbox_t::send(const command_t &cmd_)
{
    //  Attempt to write an entire command without blocking.
    ssize_t nbytes;
    do
    {
        nbytes = ::send(w, &cmd_, sizeof(command_t), 0);
    } while (nbytes == -1 && errno == EINTR);

    //  Attempt to increase mailbox SNDBUF if the send failed.
    if (nbytes == -1 && errno == EAGAIN)
    {
        int old_sndbuf, new_sndbuf;
        socklen_t sndbuf_size = sizeof old_sndbuf;

        //  Retrieve current send buffer size.
        int rc = getsockopt(w, SOL_SOCKET, SO_SNDBUF, &old_sndbuf,
                            &sndbuf_size);
        errno_assert(rc == 0);
        new_sndbuf = old_sndbuf * 2;

        //  Double the new send buffer size.
        rc = setsockopt(w, SOL_SOCKET, SO_SNDBUF, &new_sndbuf, sndbuf_size);
        errno_assert(rc == 0);

        //  Verify that the OS actually honored the request.
        rc = getsockopt(w, SOL_SOCKET, SO_SNDBUF, &new_sndbuf, &sndbuf_size);
        errno_assert(rc == 0);
        zmq_assert(new_sndbuf > old_sndbuf);

        //  Retry the sending operation; at this point it must succeed.
        do
        {
            nbytes = ::send(w, &cmd_, sizeof(command_t), 0);
        } while (nbytes == -1 && errno == EINTR);
    }
    errno_assert(nbytes != -1);

    //  This should never happen as we've already checked that command size is
    //  less than PIPE_BUF.
    zmq_assert(nbytes == sizeof(command_t));
}

int zmq::mailbox_t::recv(command_t *cmd_, bool block_)
{
#ifdef MSG_DONTWAIT

    //  Attempt to read an entire command. Returns EAGAIN if non-blocking
    //  mode is requested and a command is not available.
    ssize_t nbytes = ::recv(r, cmd_, sizeof(command_t),
                            block_ ? 0 : MSG_DONTWAIT);
    if (nbytes == -1 && (errno == EAGAIN || errno == EINTR))
        return -1;
#else

    //  If required, set the reader to blocking mode.
    if (block_)
    {
        int flags = fcntl(r, F_GETFL, 0);
        errno_assert(flags >= 0);
        int rc = fcntl(r, F_SETFL, flags & ~O_NONBLOCK);
        errno_assert(rc == 0);
    }

    //  Attempt to read an entire command. Returns EAGAIN if non-blocking
    //  and a command is not available. Save value of errno if we wish to pass
    //  it to caller.
    int err = 0;
    ssize_t nbytes = ::recv(r, cmd_, sizeof(command_t), 0);
    if (nbytes == -1 && (errno == EAGAIN || errno == EINTR))
        err = errno;

    //  Re-set the reader to non-blocking mode.
    if (block_)
    {
        int flags = fcntl(r, F_GETFL, 0);
        errno_assert(flags >= 0);
        int rc = fcntl(r, F_SETFL, flags | O_NONBLOCK);
        errno_assert(rc == 0);
    }

    //  If the recv failed, return with the saved errno if set.
    if (err != 0)
    {
        errno = err;
        return -1;
    }

#endif

    //  Sanity check for success.
    errno_assert(nbytes != -1);

    //  Check whether we haven't got half of command.
    zmq_assert(nbytes == sizeof(command_t));

    return 0;
}

int zmq::mailbox_t::make_socketpair(fd_t *r_, fd_t *w_)
{
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    errno_assert(rc == 0);
    *w_ = sv[0];
    *r_ = sv[1];
    return 0;
}
