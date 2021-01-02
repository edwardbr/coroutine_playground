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
#include "bit_fiddler.h"


namespace std 
{
    const std::string& to_string(const std::string& val)
    {
        return val;
    }
}

namespace coro_exec
{

    using namespace std::chrono_literals;

    enum class message_flags
    {
        request = 1,
        requires_reply = 2,
        continuation = 4
    };

    ENABLE_BITMASK_OPERATORS(message_flags)

    template<typename COMMAND_TYPE, typename PAYLOAD_TYPE>
    struct message
    {
        int request_id = 0;
        message_flags flags = message_flags::request | message_flags::requires_reply;
        int error_code = 0;
        COMMAND_TYPE command;
        PAYLOAD_TYPE payload;
    };

    template<typename COMMAND_TYPE, typename PAYLOAD_TYPE>
    void print_message(bool is_host, const char* location, message<COMMAND_TYPE, PAYLOAD_TYPE>& msg)
    {
        printf("%s %s msg %d is request %d requires reply %d continuation %d err_code %d command %s payload %s\n",
            (is_host ? "host" : "enclave"), 
            location, 
            msg.request_id, 
            msg.flags & message_flags::request,
            msg.flags & message_flags::requires_reply,
            msg.flags & message_flags::continuation, 
            msg.error_code,
            std::to_string(msg.command).c_str(), 
            std::to_string(msg.payload).c_str());
    }

    ///////////////////////////////////////////////////////////

    template<typename COMMAND_TYPE, typename PAYLOAD_TYPE> 
    class executor
    {
    public:
        class function_dispatcher;
        using message_type = message<COMMAND_TYPE, PAYLOAD_TYPE>;
        using resumable = cppcoro::task<message_type>;
        using callback_lookup = std::unordered_map<int, std::function<void(message_type&)>>;
        using function_dispatcher_lookup = std::unordered_map<int, function_dispatcher>;
        using garbage_collection = std::vector<int>;

    private:

        callback_lookup callback_map;
        function_dispatcher_lookup resume_map;
        garbage_collection garbage;
        spsc_queue<message_type, 200>& send_queue;
        spsc_queue<message_type, 200>& reply_queue;

        // only used by the host
        spsc_queue<message_type, 200> control_send_queue;
        spsc_queue<message_type, 200> control_reply_queue;

    public:
        // this forward declaration needs to be
        struct context;

    private:
        std::function<bool(context&, message_type&)> command_set;

        bool& finish_request;
        std::atomic<int> unique_id = 0;
        bool host = true;

        std::thread thread;

    public:
        executor(bool is_host, 
            std::function<bool(context&, message_type&)> commands, 
            spsc_queue<message_type, 200>& send_q,
            spsc_queue<message_type, 200>& receive_q, 
            bool& finish_r)
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

        void post_message(message_type& m)
        {
            m.request_id = ++unique_id;

            print_message(host, "post_message", m);

            while (!finish_request && !control_send_queue.push(m)) { }
        }

        message_type read_message()
        {
            message_type m;
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
                        message_type m;
                        while (send_queue.pop(m))
                        {
                            // print_message(host, "pop message", m);
                            if (*(m.flags & message_flags::request))
                            {
                                if (!command_set(context {*this}, m))
                                {
                                    m.error_code = 1; // we have a dodgy request so report is as such
                                    m.payload = "command not recognised";
                                    m.flags &= ~message_flags::request;
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
            void execute_command(message_type& msg, std::function<resumable(message_type&, context&)> fd)
            {
                exec.execute_command(msg, fd);
            }
            void execute_command(message_type& msg, std::function<message_type(message_type&, context&)> fd)
            {
                exec.execute_command(msg, fd);
            }
            auto send_message_async(message_type& m) { return exec.send_message_async(m); }
            auto post_message_async(message_type& m) { return exec.post_message_async(m); }
        };

    private:
        void execute_command(message_type& msg, std::function<resumable(message_type&, context&)> fd)
        {
            auto it = resume_map.emplace(msg.request_id, message_wrapper(msg, fd));
            it.first->second.resume();
        }

        void execute_command(message_type& msg, std::function<message_type(message_type&, context&)> fd)
        {
            auto it = resume_map.emplace(msg.request_id, message_wrapper(msg, fd));
            it.first->second.resume();
        }

        struct send_awaitable
        {
            executor& exec;
            message_type& request_msg;
            message_type response_msg;
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
                exec.callback_map.emplace(request_msg.request_id, [h, this](message_type& m) {
                    response_msg = m;
                    h.resume();
                });
                return true;
            }
            message_type await_resume() { return response_msg; }
        };

        send_awaitable send_message_async(message_type& m)
        {
            m.request_id = ++unique_id;
            // print_message(host, "send_message_async", m);

            return send_awaitable {*this, m};
        }

        struct post_awaitable
        {
            executor& exec;
            message_type& request_msg;
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

        post_awaitable post_message_async(message_type& m)
        {
            if (*(m.flags & message_flags::request))
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
        function_dispatcher message_wrapper(message_type& m, std::function<message_type(message_type&, context&)> fn)
        {
            auto message_id = m.request_id;
            bool requires_reply = *(m.flags & message_flags::requires_reply);
            try
            {
                message_type ret = fn(m, context {*this});

                if (requires_reply)
                {
                    ret.flags &= ~(message_flags::request | message_flags::requires_reply);
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
        function_dispatcher message_wrapper(message_type& m, std::function<resumable(message_type&, context&)> fn)
        {
            auto message_id = m.request_id;
            bool requires_reply = *(m.flags & message_flags::requires_reply);
            try
            {
                auto ret = co_await fn(m, context {*this});

                if (requires_reply)
                {
                    ret.flags &= ~(message_flags::request | message_flags::requires_reply);
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
}