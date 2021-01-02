#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <cppcoro/task.hpp>

#include "queue.h"
using namespace std::chrono_literals;

struct message
{
    int request_id = 0;
    bool is_request = true;
    bool requires_reply = true;
    int error_code = 0;
    std::string command;
    std::string payload;
};

void print_message(bool is_host, const char* location, message& msg)
{
    printf("%s %s msg %d is request %d requires reply %d err_code %d command %s payload %s\n",
           (is_host ? "host" : "enclave"), location, msg.request_id, msg.is_request, msg.requires_reply, msg.error_code,
           msg.command.c_str(), msg.payload.c_str());
}

using resumable = cppcoro::task<message>;

///////////////////////////////////////////////////////////

class executor
{
    class function_dispatcher;
    using callback_lookup = std::unordered_map<int, std::function<void(message&)>>;
    using function_dispatcher_lookup = std::unordered_map<int, function_dispatcher>;
    using garbage_collection = std::vector<int>;

    callback_lookup callback_map;
    function_dispatcher_lookup resume_map;
    garbage_collection garbage;
    spsc_queue<message, 200>& send_queue;
    spsc_queue<message, 200>& reply_queue;

    // only used by the host
    spsc_queue<message, 200> control_send_queue;
    spsc_queue<message, 200> control_reply_queue;

public:
    // this forward declaration needs to be
    struct context;

private:
    std::function<bool(context&, message&)> command_set;

    bool& finish_request;
    std::atomic<int> unique_id = 0;
    bool host = true;

    std::thread thread;

public:
    executor(bool is_host, std::function<bool(context&, message&)> commands, spsc_queue<message, 200>& send_q,
             spsc_queue<message, 200>& receive_q, bool& finish_r)
        : command_set(commands)
        , send_queue(send_q)
        , reply_queue(receive_q)
        , finish_request(finish_r)
        , host(is_host)
    {
    }

    bool calls_active() { return !resume_map.empty(); }
    bool has_recieved_calls() { return unique_id.load(std::memory_order_relaxed) != 0; }
    bool is_host() { return host; }

    void send_message_sync(message& m)
    {
        m.request_id = ++unique_id;

        print_message(host, "send_message_sync", m);

        while (!finish_request && !control_send_queue.push(m)) { }
    }

    message read_message_sync()
    {
        message m;
        while (!finish_request && !control_reply_queue.pop(m)) { }
        // print_message(host, "read_message_sync", m);
        return m;
    }

    bool is_shutting_down() { return finish_request; }

    void start()
    {
        if (thread.get_id() == std::thread::id())
        {
            thread = std::thread([&]() {
                while (!finish_request)
                {
                    message m;
                    while (send_queue.pop(m))
                    {
                        // print_message(host, "pop message", m);
                        if (m.is_request)
                        {
                            if (!command_set(context {*this}, m))
                            {
                                m.error_code = 1; // we have a dodgy request so report is as such
                                m.payload = "command not recognised";
                                m.is_request = false;
                                while (!finish_request && !reply_queue.push(m)) { }
                            }
                        }
                        else
                        {
                            auto cb = callback_map.find(m.request_id);
                            if (cb == callback_map.end())
                            {
                                if (is_host())
                                {
                                    while (!finish_request && !control_reply_queue.push(m)) { }
                                }
                                else
                                {
                                    m.error_code = 1; // we have a dodgy request so report is as such
                                    m.payload = "callback not recognised";
                                    print_message(host, "callback not recognised", m);
                                }
                            }
                            else
                            {
                                cb->second(m);
                                callback_map.erase(cb);
                            }
                        }
                        for (int i : garbage)
                        {
                            resume_map.erase(i);
                        }
                        garbage.clear();
                    }
                    while (host && control_send_queue.pop(m))
                    {
                        // print_message(host, "control pop message", m);
                        while (!reply_queue.push(m))
                        {
                            if (is_shutting_down())
                            {
                                break;
                            }
                        }
                    }
                }
            });
        }
        else
        {
            assert(false);
        }
    }

    void join()
    {
        if (thread.get_id() != std::thread::id())
        {
            thread.join();
        }
    }

    struct context
    {
        executor& exec;
        void execute_command(message& msg, std::function<resumable(message&, context&)> fd)
        {
            exec.execute_command(msg, fd);
        }
        void execute_command(message& msg, std::function<message(message&, context&)> fd)
        {
            exec.execute_command(msg, fd);
        }
        auto send_message_async(message& m) { return exec.send_message_async(m); }
        auto post_message_async(message& m) { return exec.post_message_async(m); }
    };

private:
    void execute_command(message& msg, std::function<resumable(message&, context&)> fd)
    {
        auto it = resume_map.emplace(msg.request_id, message_wrapper(msg, fd));
        it.first->second.resume();
    }

    void execute_command(message& msg, std::function<message(message&, context&)> fd)
    {
        auto it = resume_map.emplace(msg.request_id, message_wrapper(msg, fd));
        it.first->second.resume();
    }

    struct send_awaitable
    {
        executor& exec;
        message& request_msg;
        message response_msg;
        bool await_ready() { return false; }
        bool await_suspend(cppcoro::coroutine_handle<> h)
        {
            if (exec.is_shutting_down())
            {
                return false;
            }
            while (!exec.reply_queue.push(request_msg))
            {
                if (exec.is_shutting_down())
                {
                    return false;
                }
            }
            exec.callback_map.emplace(request_msg.request_id, [h, this](message& m) {
                response_msg = m;
                h.resume();
            });
            return true;
        }
        message await_resume() { return response_msg; }
    };

    send_awaitable send_message_async(message& m)
    {
        m.request_id = ++unique_id;
        // print_message(host, "send_message_async", m);

        return send_awaitable {*this, m};
    }

    struct post_awaitable
    {
        executor& exec;
        message& request_msg;
        bool await_ready() { return false; }
        bool await_suspend(cppcoro::coroutine_handle<> h)
        {
            if (exec.is_shutting_down())
            {
                return false;
            }
            while (!exec.reply_queue.push(request_msg))
            {
                if (exec.is_shutting_down())
                {
                    return false;
                }
            }
            return false;
        }
        void await_resume() { }
    };

    post_awaitable post_message_async(message& m)
    {
        if (m.is_request)
        {
            m.request_id = ++unique_id;
        }

        // print_message(host, "post_message_async", m);

        return post_awaitable {*this, m};
    }

    class function_dispatcher
    {
    public:
        struct promise_type
        {
            using coro_handle = cppcoro::coroutine_handle<promise_type>;
            coro_handle get_return_object() { return coro_handle::from_promise(*this); }
            auto initial_suspend() { return cppcoro::suspend_always(); }
            auto final_suspend() noexcept { return cppcoro::suspend_always(); }
            void return_void() { }
            void unhandled_exception() { std::terminate(); }
        };
        using coro_handle = cppcoro::coroutine_handle<promise_type>;
        function_dispatcher(coro_handle handle)
            : handle_(handle)
        {
            assert(handle);
        }
        function_dispatcher(function_dispatcher&) = delete;
        function_dispatcher(function_dispatcher&& other) { std::swap(handle_, other.handle_); }
        bool resume()
        {
            if (!handle_.done())
                handle_.resume();
            return !handle_.done();
        }
        ~function_dispatcher()
        {
            if (handle_)
            {
                handle_.destroy();
            }
        }

    private:
        coro_handle handle_;
    };

    // for non awaitables
    function_dispatcher message_wrapper(message& m, std::function<message(message&, context&)> fn)
    {
        auto message_id = m.request_id;
        bool requires_reply = m.requires_reply;
        try
        {
            message ret = fn(m, context {*this});

            if (requires_reply)
            {
                ret.is_request = false;
                ret.requires_reply = false;
                co_await this->post_message_async(ret);
            }
        }
        catch (...)
        {
            garbage.push_back(message_id);
            throw;
        }

        garbage.push_back(message_id);
        co_return;
    }

    // for co_awaitables
    function_dispatcher message_wrapper(message& m, std::function<resumable(message&, context&)> fn)
    {
        auto message_id = m.request_id;
        bool requires_reply = m.requires_reply;
        try
        {
            auto ret = co_await fn(m, context {*this});

            if (requires_reply)
            {
                ret.is_request = false;
                ret.requires_reply = false;
                co_await this->post_message_async(ret);
            }
        }
        catch (...)
        {
            garbage.push_back(message_id);
            throw;
        }
        garbage.push_back(message_id);
        co_return;
    }

    friend context;
};
