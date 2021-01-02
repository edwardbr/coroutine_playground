#include <thread>
#include <vector>
#include <atomic>
#include <string>
#include <iostream>
#include <functional>
#include <unordered_map>
#include <future>
#include <chrono>
#include <assert.h>

#include <cppcoro/task.hpp>
using namespace std::chrono_literals;

template<typename T, size_t Size> class spsc_queue
{
public:
    spsc_queue()
        : head_(0)
        , tail_(0)
    {
    }

    bool push(const T& value)
    {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = next(head);
        if (next_head == tail_.load(std::memory_order_acquire))
            return false;
        ring_[head] = value;
        head_.store(next_head, std::memory_order_release);
        return true;
    }
    bool pop(T& value)
    {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        value = ring_[tail];
        tail_.store(next(tail), std::memory_order_release);
        return true;
    }

private:
    size_t next(size_t current) { return (current + 1) % Size; }
    T ring_[Size];
    std::atomic<size_t> head_, tail_;
};

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
    printf("%s %s msg %d is request %d requires reply %d err_code %d command %s payload %s\n", (is_host ? "host" : "enclave"), location, msg.request_id,
           msg.is_request, msg.requires_reply, msg.error_code, msg.command.c_str(), msg.payload.c_str());
}

using resumable = cppcoro::task<message>;

///////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////

class executor;
message echo_world(message& m, executor& exec);
resumable double_echo(message& m, executor& exec);
resumable ping(message& m, executor& exec);

struct awaitable_message;

class executor
{
    using callback_lookup = std::unordered_map<int, std::function<void(message&)>>;
    using function_dispatcher_lookup = std::unordered_map<int, function_dispatcher>;
    using garbage_collection = std::vector<int>;
    friend awaitable_message;

    callback_lookup callback_map;
    function_dispatcher_lookup resume_map;
    garbage_collection garbage;
    spsc_queue<message, 200>& send_queue;
    spsc_queue<message, 200>& reply_queue;

    // only used by the host
    spsc_queue<message, 200> control_send_queue;
    spsc_queue<message, 200> control_reply_queue;

    bool& finish_request;
    std::atomic<int> unique_id = 0;
    bool host = true;

public:
    executor(bool is_host, spsc_queue<message, 200>& send_q, spsc_queue<message, 200>& receive_q, bool& finish_r)
        : send_queue(send_q)
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
        while (!finish_request && !control_reply_queue.pop(m)) 
        {}
        print_message(host, "read_message_sync", m);
        return m;
    }

    auto send_message_async(message& m)
    {
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

        m.request_id = ++unique_id;
        //print_message(host, "send_message_async", m);

        return send_awaitable {*this, m};
    }

    auto post_message_async(message& m)
    {
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

        if (m.is_request)
        {
            m.request_id = ++unique_id;
        }

        //print_message(host, "post_message_async", m);

        return post_awaitable {*this, m};
    }

    bool is_shutting_down() { return finish_request; }

    // for non awaitables
    function_dispatcher message_wrapper(message& m, std::function<message(message&, executor&)> fn)
    {
        auto message_id = m.request_id;
        bool requires_reply = m.requires_reply;
        try
        {
            message ret = fn(m, *this);

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
    function_dispatcher message_wrapper(message& m, std::function<resumable(message&, executor&)> fn)
    {
        auto message_id = m.request_id;
        bool requires_reply = m.requires_reply;
        try
        {
            auto ret = co_await fn(m, *this);

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

    std::thread run_thread()
    {
        std::thread t([&]() {
            while (!finish_request)
            {
                message m;
                while (send_queue.pop(m))
                {
                    print_message(host, "pop message", m);
                    if (m.is_request)
                    {
                        std::pair<function_dispatcher_lookup::iterator, bool> it;
                        if (m.command == "hello")
                        {
                            it = resume_map.emplace(m.request_id, message_wrapper(m, echo_world));
                        }
                        else if (m.command == "ping")
                        {
                            it = resume_map.emplace(m.request_id, message_wrapper(m, ping));
                        }
                        else if (m.command == "double_echo")
                        {
                            it = resume_map.emplace(m.request_id, message_wrapper(m, double_echo));
                        }                        
                        else
                        {
                            m.error_code = 1; // we have a dodgy request so report is as such
                            m.payload = "command not recognised";
                            m.is_request = false;
                            while (!finish_request && !reply_queue.push(m)) { }
                        }

                        it.first->second.resume();
                    }
                    else
                    {
                        auto cb = callback_map.find(m.request_id);
                        if (cb == callback_map.end())
                        {
                            if(is_host())
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
                    //print_message(host, "control pop message", m);
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
        return t;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////

//non coroutine
message echo_world(message& m, executor& exec)
{
    message msg = m;
    msg.payload += " world";
    msg.is_request = false;
    msg.requires_reply = false;
    return msg;
}

//a coroutine calling an awaitable
resumable ping(message& m, executor& exec)
{
    message reply = m;
    reply.is_request = false;
    reply.requires_reply = false;

    {
      message query {0, true, true, 0, "hello", "hello"};
      auto ret = co_await exec.send_message_async(query);
      reply.payload = "pong " + ret.payload;
    }
    co_return reply;
}

//coroutine calling another coroutine
resumable double_echo(message& m, executor& exec)
{
    message reply = m;
    reply.is_request = false;
    reply.requires_reply = false;

    message ret = co_await ping(m, exec);
    co_return reply;
}

int main()
{
    bool finish_request = false;
    spsc_queue<message, 200> send_queue;
    spsc_queue<message, 200> reply_queue;

    executor enclave_exec(false, send_queue, reply_queue, finish_request);
    auto enclave_thread = enclave_exec.run_thread();

    executor host_exec(true, reply_queue, send_queue, finish_request);
    auto host_thread = host_exec.run_thread();

    int count = 0;
    {
        message m {0, true, true, 0, "hello", "hello"};
        host_exec.send_message_sync(m);
        count++;
    }
    {
        message m {1, true, true, 0, "ping", ""};
        host_exec.send_message_sync(m);
        count++;
    }
    {
        message m {1, true, true, 0, "double_echo", ""};
        host_exec.send_message_sync(m);
        count++;
    }
    {
        message m;
        while(count)
        {
            message m = host_exec.read_message_sync();
            print_message(true, "main", m);
            count--;
        }
    }

    finish_request = true;

    enclave_thread.join();
    host_thread.join();

    return 0;
}