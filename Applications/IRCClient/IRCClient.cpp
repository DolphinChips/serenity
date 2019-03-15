#include "IRCClient.h"
#include "IRCChannel.h"
#include "IRCQuery.h"
#include "IRCLogBuffer.h"
#include "IRCClientWindow.h"
#include <LibGUI/GNotifier.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>

enum IRCNumeric {
    RPL_NAMREPLY = 353,
    RPL_ENDOFNAMES = 366,
};

IRCClient::IRCClient(const String& address, int port)
    : m_hostname(address)
    , m_port(port)
    , m_nickname("anon")
    , m_log(IRCLogBuffer::create())
{
}

IRCClient::~IRCClient()
{
}

bool IRCClient::connect()
{
    if (m_socket_fd != -1) {
        ASSERT_NOT_REACHED();
    }

    m_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket_fd < 0) {
        perror("socket");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    int rc = inet_pton(AF_INET, m_hostname.characters(), &addr.sin_addr);
    if (rc < 0) {
        perror("inet_pton");
        exit(1);
    }

    printf("Connecting to %s...", m_hostname.characters());
    fflush(stdout);
    rc = ::connect(m_socket_fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc < 0) {
        perror("connect");
        exit(1);
    }
    printf("ok!\n");

    m_notifier = make<GNotifier>(m_socket_fd, GNotifier::Read);
    m_notifier->on_ready_to_read = [this] (GNotifier&) { receive_from_server(); };

    if (on_connect)
        on_connect();

    send_user();
    send_nick();
    return true;
}

void IRCClient::receive_from_server()
{
    char buffer[4096];
    int nread = recv(m_socket_fd, buffer, sizeof(buffer) - 1, 0);
    if (nread < 0) {
        perror("recv");
        exit(1);
    }
    if (nread == 0) {
        printf("IRCClient: Connection closed!\n");
        exit(1);
    }
    buffer[nread] = '\0';
#if 0
    printf("Received: '%s'\n", buffer);
#endif

    for (int i = 0; i < nread; ++i) {
        char ch = buffer[i];
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            process_line();
            m_line_buffer.clear_with_capacity();
            continue;
        }
        m_line_buffer.append(ch);
    }
}

void IRCClient::process_line()
{
#if 0
    printf("Process line: '%s'\n", line.characters());
#endif
    Message msg;
    Vector<char> prefix;
    Vector<char> command;
    Vector<char> current_parameter;
    enum {
        Start,
        InPrefix,
        InCommand,
        InStartOfParameter,
        InParameter,
        InTrailingParameter,
    } state = Start;

    for (int i = 0; i < m_line_buffer.size(); ++i) {
        char ch = m_line_buffer[i];
        switch (state) {
        case Start:
            if (ch == ':') {
                state = InPrefix;
                continue;
            }
            state = InCommand;
            [[fallthrough]];
        case InCommand:
            if (ch == ' ') {
                state = InStartOfParameter;
                continue;
            }
            command.append(ch);
            continue;
        case InPrefix:
            if (ch == ' ') {
                state = InCommand;
                continue;
            }
            prefix.append(ch);
            continue;
        case InStartOfParameter:
            if (ch == ':') {
                state = InTrailingParameter;
                continue;
            }
            state = InParameter;
            [[fallthrough]];
        case InParameter:
            if (ch == ' ') {
                if (!current_parameter.is_empty())
                    msg.arguments.append(String(current_parameter.data(), current_parameter.size()));
                current_parameter.clear_with_capacity();
                state = InStartOfParameter;
                continue;
            }
            current_parameter.append(ch);
            continue;
        case InTrailingParameter:
            current_parameter.append(ch);
            continue;
        }
    }
    if (!current_parameter.is_empty())
        msg.arguments.append(String(current_parameter.data(), current_parameter.size()));
    msg.prefix = String(prefix.data(), prefix.size());
    msg.command = String(command.data(), command.size());
    handle(msg, String(m_line_buffer.data(), m_line_buffer.size()));
}

void IRCClient::send(const String& text)
{
    int rc = ::send(m_socket_fd, text.characters(), text.length(), 0);
    if (rc < 0) {
        perror("send");
        exit(1);
    }
}

void IRCClient::send_user()
{
    send(String::format("USER %s 0 * :%s\r\n", m_nickname.characters(), m_nickname.characters()));
}

void IRCClient::send_nick()
{
    send(String::format("NICK %s\r\n", m_nickname.characters()));
}

void IRCClient::send_pong(const String& server)
{
    send(String::format("PONG %s\r\n", server.characters()));
    sleep(1);
}

void IRCClient::join_channel(const String& channel_name)
{
    send(String::format("JOIN %s\r\n", channel_name.characters()));
}

void IRCClient::handle(const Message& msg, const String& verbatim)
{
    printf("IRCClient::execute: prefix='%s', command='%s', arguments=%d\n",
        msg.prefix.characters(),
        msg.command.characters(),
        msg.arguments.size()
    );

    int i = 0;
    for (auto& arg : msg.arguments) {
        printf("    [%d]: %s\n", i, arg.characters());
        ++i;
    }

    bool is_numeric;
    int numeric = msg.command.to_uint(is_numeric);

    if (is_numeric) {
        switch (numeric) {
        case RPL_NAMREPLY:
            handle_namreply(msg);
            return;
        }
    }

    if (msg.command == "PING")
        return handle_ping(msg);

    if (msg.command == "JOIN")
        return handle_join(msg);

    if (msg.command == "PRIVMSG")
        return handle_privmsg(msg);

    if (msg.arguments.size() >= 2)
        m_log->add_message(0, "Server", String::format("[%s] %s", msg.command.characters(), msg.arguments[1].characters()));
}

bool IRCClient::is_nick_prefix(char ch) const
{
    switch (ch) {
    case '@':
    case '+':
    case '~':
    case '&':
    case '%':
        return true;
    }
    return false;
}

void IRCClient::handle_privmsg(const Message& msg)
{
    if (msg.arguments.size() < 2)
        return;
    if (msg.prefix.is_empty())
        return;
    auto parts = msg.prefix.split('!');
    auto sender_nick = parts[0];
    auto target = msg.arguments[0];

    printf("handle_privmsg: sender_nick='%s', target='%s'\n", sender_nick.characters(), target.characters());

    if (sender_nick.is_empty())
        return;

    char sender_prefix = 0;
    if (is_nick_prefix(sender_nick[0])) {
        sender_prefix = sender_nick[0];
        sender_nick = sender_nick.substring(1, sender_nick.length() - 1);
    }

    {
        auto it = m_channels.find(target);
        if (it != m_channels.end()) {
            (*it).value->add_message(sender_prefix, sender_nick, msg.arguments[1]);
            if (on_channel_message)
                on_channel_message(target);
            return;
        }
    }
    auto& query = ensure_query(sender_nick);
    query.add_message(sender_prefix, sender_nick, msg.arguments[1]);
    if (on_query_message)
        on_query_message(target);
}

IRCQuery& IRCClient::ensure_query(const String& name)
{
    auto it = m_queries.find(name);
    if (it != m_queries.end())
        return *(*it).value;
    auto query = IRCQuery::create(*this, name);
    auto& query_reference = *query;
    m_queries.set(name, query.copy_ref());
    return query_reference;
}

void IRCClient::handle_ping(const Message& msg)
{
    if (msg.arguments.size() < 0)
        return;
    m_log->add_message(0, "", String::format("Ping? Pong! %s\n", msg.arguments[0].characters()));
    send_pong(msg.arguments[0]);
}

void IRCClient::handle_join(const Message& msg)
{
    if (msg.arguments.size() != 1)
        return;
    auto& channel_name = msg.arguments[0];
    auto it = m_channels.find(channel_name);
    ASSERT(it == m_channels.end());
    auto channel = IRCChannel::create(*this, channel_name);
    m_channels.set(channel_name, move(channel));
}

void IRCClient::handle_namreply(const Message& msg)
{
    printf("NAMREPLY:\n");
    if (msg.arguments.size() < 4)
        return;

    auto& channel_name = msg.arguments[2];

    auto it = m_channels.find(channel_name);
    if (it == m_channels.end()) {
        fprintf(stderr, "Warning: Got RPL_NAMREPLY for untracked channel %s\n", channel_name.characters());
        return;
    }
    auto& channel = *(*it).value;

    auto members = msg.arguments[3].split(' ');
    for (auto& member : members) {
        if (member.is_empty())
            continue;
        char prefix = 0;
        if (is_nick_prefix(member[0]))
            prefix = member[0];
        channel.add_member(member, prefix);
    }

    channel.dump();
}

void IRCClient::register_subwindow(IRCClientWindow& subwindow)
{
    if (subwindow.type() == IRCClientWindow::Server) {
        m_server_subwindow = &subwindow;
        subwindow.set_log_buffer(*m_log);
        return;
    }
    if (subwindow.type() == IRCClientWindow::Channel) {
        auto it = m_channels.find(subwindow.name());
        ASSERT(it != m_channels.end());
        auto& channel = *(*it).value;
        subwindow.set_log_buffer(channel.log());
        return;
    }
    if (subwindow.type() == IRCClientWindow::Query) {
        subwindow.set_log_buffer(ensure_query(subwindow.name()).log());
    }
}

void IRCClient::unregister_subwindow(IRCClientWindow& subwindow)
{
    if (subwindow.type() == IRCClientWindow::Server) {
        m_server_subwindow = &subwindow;
        return;
    }
}
