#include "executor.h"

////////////////////////////////////////////////////////////////////////////////////////////

using executor = coro_exec::executor<std::string, std::string>;
using message = executor::message_type;
using execution_context = executor::context;
using resumable = executor::resumable;

// non coroutine
message echo_world(message& m, execution_context& exec)
{
    message msg = m;
    msg.payload += " world";
    msg.flags &= ~(coro_exec::message_flags::request | coro_exec::message_flags::requires_reply);
    return msg;
}

// a coroutine calling an awaitable
resumable ping(message& m, execution_context& exec)
{
    message reply = m;
    reply.flags &= ~(coro_exec::message_flags::request | coro_exec::message_flags::requires_reply);

    {
        message query {0, coro_exec::message_flags::request | coro_exec::message_flags::requires_reply, 0, "hello", "hello"};
        auto ret = co_await exec.send_message_async(query);
        reply.payload = "pong " + ret.payload;
    }
    co_return reply;
}

// coroutine calling another coroutine
resumable double_echo(message& m, execution_context& exec)
{
    message reply = m;
    reply.flags &= ~(coro_exec::message_flags::request | coro_exec::message_flags::requires_reply);

    message ret = co_await ping(m, exec);
    co_return reply;
}

int main()
{
    bool finish_request = false;
    spsc_queue<message, 200> send_queue;
    spsc_queue<message, 200> reply_queue;

    auto commands = [](execution_context& exec, message& m) -> bool {
        if (m.command == "hello")
        {
            exec.execute_command(m, echo_world);
        }
        else if (m.command == "ping")
        {
            exec.execute_command(m, ping);
        }
        else if (m.command == "double_echo")
        {
            exec.execute_command(m, double_echo);
        }
        else
        {
            return false;
        }

        return true;
    };

    executor enclave_exec(false, commands, send_queue, reply_queue, finish_request);
    enclave_exec.start();

    executor host_exec(true, commands, reply_queue, send_queue, finish_request);
    host_exec.start();

    int count = 0;
    {
        message m {0, coro_exec::message_flags::request | coro_exec::message_flags::requires_reply, 0, "hello", "hello"};
        host_exec.post_message(m);
        count++;
    }
    {
        message m {1, coro_exec::message_flags::request | coro_exec::message_flags::requires_reply, 0, "ping", ""};
        host_exec.post_message(m);
        count++;
    }
    {
        message m {1, coro_exec::message_flags::request | coro_exec::message_flags::requires_reply, 0, "double_echo", ""};
        host_exec.post_message(m);
        count++;
    }
    {
        message m;
        while (count)
        {
            message m = host_exec.read_message();
            print_message(true, "main", m);
            count--;
        }
    }

    finish_request = true;

    enclave_exec.join();
    host_exec.join();

    return 0;
}