#include "executor.h"

////////////////////////////////////////////////////////////////////////////////////////////

using executor = coro_exec::executor<std::string, std::string>;
using message = executor::message_type;
using execution_context = executor::context;
using resumable = executor::resumable;

// non coroutine
executor::return_type echo_world(std::string& payload, execution_context& exec)
{
    return {payload += " world", 0};
}

// a coroutine calling an awaitable
resumable ping(std::string& payload, execution_context& exec)
{
    executor::return_type ret = co_await exec.send_message_async(std::string("hello"), std::string("hello"));
    co_return executor::return_type{std::string("pong ") + ret.first, 0};
}

// coroutine calling another coroutine
resumable double_echo(std::string& payload, execution_context& exec)
{
    executor::return_type reply = co_await ping(payload, exec);
    co_return executor::return_type{reply.first, 0};
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