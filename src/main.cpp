#include <coroutine>
#include <optional>

#include <iostream>
#include <thread>

#include <chrono>
#include <queue>
#include <vector>

#include <event2/event.h>

event_base* g_event_loop = nullptr;

auto _event_base_free = [](event_base *p) { event_base_free(p); };
auto _event_free = [](event *p) { event_free(p); };

#include <ctime>

#define trace(...) _trace(__FILE__, __LINE__, ##__VA_ARGS__)

template <typename ...A>
static inline void _trace(char const* file, int line, A&&... Args)
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    tm loctime;
    localtime_r(&ts.tv_sec, &loctime);

    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S", &loctime);

    std::clog << buf << "." << (ts.tv_nsec / 1000) << " (" << file << ":" << line << ")"; // << " [" << loc.function_name() << "]";

    if constexpr (sizeof...(A) > 0)
    {
        std::clog << ": ";
        ((std::clog << std::forward<A>(Args) << " "), ...);
    }

    std::clog << "\n";
}

// basic coroutine single-threaded async task example

struct awaitable_event
{
    int fd;
    short what;
    int timeout;
};

template<typename T>
struct task_promise_type;

// simple single-threaded timer for coroutines
void submit_timer_task(std::coroutine_handle<> handle, std::chrono::seconds timeout);

template<typename T>
struct task;

template<typename T>
struct task_promise_type
{
    task_promise_type() = default;
    ~task_promise_type() {trace("destroy meeee", value.value_or(T{}));}

    // value to be computed
    // when task is not completed (coroutine didn't co_return anything yet) value is empty
    std::optional<T> value;

    // corouine that awaiting this coroutine value
    // we need to store it in order to resume it later when value of this coroutine will be computed
    std::coroutine_handle<> awaiting_coroutine;

    // task is async result of our coroutine
    // it is created before execution of the coroutine body
    // it can be either co_awaited inside another coroutine
    // or used via special interface for extracting values (is_ready and get)
    task<T> get_return_object();

    // there are two kinds of coroutines:
    // 1. eager - that start its execution immediately
    // 2. lazy - that start its execution only after 'co_await'ing on them
    // here I used eager coroutine task
    // eager: do not suspend before running coroutine body
    std::suspend_never initial_suspend()
    {
        return {};
    }

    // store value to be returned to awaiting coroutine or accessed through 'get' function
    template <typename U>
    void return_value(U&& val)
    {
        trace("set return value", std::forward<U>(val));
        value = std::forward<U>(val);
    }

    void unhandled_exception()
    {
        // alternatively we can store current exeption in std::exception_ptr to rethrow it later
        std::terminate();
    }

    // when final suspend is executed 'value' is already set
    // we need to suspend this coroutine in order to use value in other coroutine or through 'get' function
    // otherwise promise object would be destroyed (together with stored value) and one couldn't access task result
    // value
    auto final_suspend() noexcept
    {
        trace("final suspend");

        // if there is a coroutine that is awaiting on this coroutine resume it
        struct transfer_awaitable
        {
            std::coroutine_handle<> awaiting_coroutine;

            // always stop at final suspend
            bool await_ready() noexcept
            {
                return false;
            }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<task_promise_type> h) noexcept
            {
                // resume awaiting coroutine or if there is no coroutine to resume return special coroutine that do
                // nothing
                return awaiting_coroutine ? awaiting_coroutine : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        return transfer_awaitable{awaiting_coroutine};
    }

    // there are multiple ways to add co_await into coroutines
    // I used `await_transform`

    // use `co_await std::chrono::seconds{n}` to wait specified amount of time
    auto await_transform(std::chrono::seconds duration)
    {
        struct timer_awaitable
        {
            std::chrono::seconds duration;
            // always suspend
            bool await_ready()
            {
                return false;
            }

            // h is a handler for current coroutine which is suspended
            void await_suspend(std::coroutine_handle<task_promise_type> h)
            {
                // submit suspended coroutine to be resumed after timeout
                submit_timer_task(h, duration);
            }
            void await_resume() {}
        };

        return timer_awaitable{duration};
    }

    // also we can await other task<T>
    template<typename U>
    auto await_transform(const task<U>& task)
    {
        if (!task.handle) {
            throw std::runtime_error("coroutine without promise awaited");
        }
        if (task.handle.promise().awaiting_coroutine) {
            throw std::runtime_error("coroutine already awaited");
        }

        struct task_awaitable
        {
            std::coroutine_handle<task_promise_type<U>> handle;

            // check if this task already has value computed
            bool await_ready()
            {
                if (handle.promise().value.has_value())
                {
                    trace("awaiting another task", handle.address(), "ready:", handle.promise().value.value());
                }
                else
                {
                    trace("awaiting another task", handle.address(), "not ready");
                }

                return handle.promise().value.has_value();
            }

            // h - is a handle to coroutine that calls co_await
            // store coroutine handle to be resumed after computing task value
            void await_suspend(std::coroutine_handle<> h)
            {
                trace("awaiting another task", handle.address(), "suspend:", h.address());
                handle.promise().awaiting_coroutine = h;
            }

            // when ready return value to a consumer
            auto await_resume()
            {
                trace("awaiting another task", handle.address(), "resume");
                return std::move(*(handle.promise().value));
            }
        };

        return task_awaitable{task.handle};
    }

    auto await_transform(awaitable_event ev)
    {
        struct event_awaitable
        {
            awaitable_event aev;
            event* ev;
            std::coroutine_handle<> handle;

            static void resume_on_event(int fd, short what, void *arg)
            {
                event_awaitable* eva = (event_awaitable*)arg;

                trace("resume", arg);
                eva->aev.what = what;
                eva->handle.resume();
            }

            bool await_ready()
            {
                return false;
            }

            void await_suspend(std::coroutine_handle<> h)
            {
                trace("awaiting event", aev.what & EV_READ ? "read" : "", aev.what & EV_WRITE ? "write" : "");

                this->handle = h;
                this->ev = event_new(g_event_loop, aev.fd, aev.what | EV_TIMEOUT, event_awaitable::resume_on_event, this);

                if (aev.timeout != 0)
                {
                    timeval tv{aev.timeout, 0};
                    event_add(this->ev, &tv);
                }
                else
                {
                    event_add(this->ev, nullptr);
                }
            }

            // when ready return value to a consumer
            bool await_resume()
            {
                event_free(this->ev);
                return (aev.what & EV_TIMEOUT) == 0;
            }
        };

        return event_awaitable{ev};
    }
};

template<typename T>
struct task
{
    // declare promise type
    using promise_type = task_promise_type<T>;

    task(std::coroutine_handle<promise_type> handle) : handle(handle) {}

    task(task&& other) : handle(std::exchange(other.handle, nullptr)) {}

    task& operator=(task&& other)
    {
        trace("task operator=");

        if (handle) {
            handle.destroy();
        }
        handle = other.handle;
    }

    ~task()
    {
        if (handle) {
            handle.destroy();
        }
    }

    // interface for extracting value without awaiting on it

    bool is_ready() const
    {
        if (handle) {
            return handle.promise().value.has_value();
        }
        return false;
    }

    T get()
    {
        if (handle) {
            return std::move(*handle.promise().value);
        }
        throw std::runtime_error("get from task without promise");
    }

    std::coroutine_handle<promise_type> handle;
};

template<typename T>
task<T> task_promise_type<T>::get_return_object()
{
    return {std::coroutine_handle<task_promise_type>::from_promise(*this)};
}

// simple timers

// stored timer tasks
struct timer_task
{
    std::unique_ptr<event, decltype(_event_free)> evtimer;
    std::coroutine_handle<> handle;
};

void invoke_resume(int fd, short flag, void *arg)
{
    timer_task* t = reinterpret_cast<timer_task*>(arg);

    t->handle.resume();

    delete t;
}

void submit_timer_task(std::coroutine_handle<> handle, std::chrono::seconds timeout)
{
    trace("timers submit", timeout.count(), handle.address());
    auto *c = new timer_task{nullptr, handle};
    c->evtimer.reset(evtimer_new(g_event_loop, invoke_resume, c));

    timeval tv {timeout.count(), 0};
    evtimer_add(c->evtimer.get(), &tv);
}

using namespace std::chrono_literals;

task<int> async_accept(int fd)
{
    if (co_await awaitable_event{fd, EV_READ, 3})
    {
        int newfd = accept(fd, nullptr, nullptr);

        trace("ok return", newfd);
        co_return newfd;
    }

    trace("timeout return");
    co_return -1;
}

task<int> async_read(int fd, char* buf, size_t bufsz)
{
    if (co_await awaitable_event{fd, EV_READ, 0})
    {
        trace("read ready");
        co_return read(fd, buf, bufsz);
    }

    co_return -1;
}

task<int> async_echo_client(int fd)
{
    trace("echo client started", fd);
    while (true)
    {
        char buf[512];
        int n = co_await async_read(fd, buf, sizeof(buf));

        if (n == 0)
        {
            break;
        }

        send(fd, buf, n, 0);
    }

    co_return 0;
}

task<int> wait_n(int n)
{
    trace("before wait", n);
    co_await std::chrono::seconds(n);
    trace("after wait", n);
    co_return n;
}

task<int> test()
{
#if 0
    for (auto c : "hello world\n") {
        trace(c);
        co_await 1s;
    }
#endif

    trace("test step 1");
    auto w3 = wait_n(3);
    trace("test step 2");
    auto w2 = wait_n(2);
    trace("test step 3");
    auto w1 = wait_n(1);
    trace("test step 4");

    auto r = co_await w2;
    trace("awaiting already computed coroutine: waited ", r);

    r += co_await w3;
    trace("awaiting already computed coroutine: waited ", r);

    r += co_await w1;
    trace("awaiting already computed coroutine: waited ", r);

    co_return r;
}

task<int> start_server()
{
    int fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in sin;
    socklen_t sinsz = sizeof(sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = 0;

    if (-1 == bind(fd, (const sockaddr *)&sin, sinsz))
    {
        perror("bind");
        co_return EXIT_FAILURE;
    }

    if (-1 == listen(fd, 42))
    {
        perror("listen");
        co_return EXIT_FAILURE;
    }

    getsockname(fd, (sockaddr *)&sin, &sinsz);

    trace("server bound to", ntohs(sin.sin_port));

    std::vector<task<int>> v;

    while (true)
    {
        int cfd = co_await async_accept(fd);

        if (cfd != -1)
        {
            trace("accepted client", cfd);

            v.emplace_back(async_echo_client(cfd));
        }
    }

    co_return EXIT_SUCCESS;
}

// main can't be a coroutine and usually need some sort of looper (io_service or timer loop in this example )
int main()
{
    std::unique_ptr<event_base, decltype(_event_base_free)> evb{event_base_new()};
    g_event_loop = evb.get();

    //auto result = test();

    auto result = start_server();

    // execute deferred coroutines
    event_base_dispatch(evb.get());

    //trace("result", result.get());
    return 0;
}