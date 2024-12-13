#pragma once
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <condition_variable>
#include <expected>
#include <mutex>
#include <stdexcept>

using tl::expected;
using tl::unexpected;
namespace cvk{

template<typename T>
concept FutureValue
=   std::is_constructible<T>::value
and std::is_move_constructible_v<T>;

namespace details{

    template <FutureValue T>
    struct shared_state{
        bool valid = false;
        bool used = false;
        std::atomic_uint8_t alive = 1;
        std::function<void(expected<T,std::exception_ptr>&&)> callback = nullptr;
        expected<T,std::exception_ptr> expected;
        asio::io_context* context = nullptr;
        std::mutex mutex;
        std::condition_variable cond_var;
    };
}


template <FutureValue U>
class promise;

template <FutureValue T>
class future{
    template <FutureValue U>
    friend class cvk::promise;

    details::shared_state<T>* state = nullptr;
    // * sync with get_future
    explicit future(details::shared_state<T>* state):state(state){state->alive.fetch_add(1,std::memory_order_relaxed);}

    future(future&) = delete; 
    void operator=(future&other) = delete;
public:
    future(future&&o):state(o.state){o.state = nullptr;}
    void operator=(future&&other){
        // //assert(not state);
        this->state = other.state;
        other.state = nullptr;
    }
   
    void subscribe(std::function<void(expected<T,std::exception_ptr>&&)>callback, asio::io_context& context){
        if(not state){throw std::logic_error("use moved future");}
        std::unique_lock lock(state->mutex);
        if(state->used){throw std::logic_error("future use second time");}
        state->used = true;
        if(state->valid){
            callback(std::move(state->expected));
            return;
        }
        state->context = &context;
        state->callback = std::move(callback);
    }
    T get(){
        if(not state){throw std::logic_error("use moved future");}
        std::unique_lock lock(state->mutex);
        if(state->used){throw std::logic_error("future use second time");}
        state->used = true;
        state->cond_var.wait(lock,[this](){return state->alive.load(std::memory_order_relaxed) == 1 or state->valid == true;});
        if(not state->valid){throw std::logic_error("future empty");}
        if(not state->expected.has_value()){std::rethrow_exception(state->expected.error());}
        return std::move(state->expected.value());
    }
    ~future(){
        if(state and 1 == state->alive.exchange(1,std::memory_order_relaxed)){
            delete state;
            return;
        }
    }
};

template <FutureValue T>
class promise{
    details::shared_state<T>* state = new details::shared_state<T>();
    // * async read-call operation under top-level mutex
    void invoke_Callback(){
        if(not state->used and not state->context){
            return;
        }
        assert(state->used);
        assert(state->valid);
        assert(state->context);
        assert(state->callback);
        asio::post(*state->context,[callback = std::move(state->callback), expected = std::move(state->expected)]() mutable{
            callback(std::move(expected));
        });
    }
    promise(promise&) = delete; 
    void operator=(promise&other) = delete;
public:
    promise(){}
    promise(promise&&o):state(o.state){o.state = nullptr;}
    void operator=(promise&&other){
        // //assert(not state);
        this->state = other.state;
        other.state = nullptr;
    }
    // * async read-write-call operation
    void set_value(T&& value){
        if(not state){throw std::logic_error("use moved promise");}
        std::unique_lock lock(state->mutex);
        if (state->valid) throw std::logic_error("value or exception already set");
        state->expected = std::move(value); 
        state->valid = true;
        invoke_Callback();
        state->cond_var.notify_one();
    }
    // * async read-write-call operation
    void set_exception(std::exception_ptr&& exc){
        if(not state){throw std::logic_error("use moved promise");}
        std::unique_lock lock(state->mutex);
        if (state->valid) throw std::logic_error("value or exception already set");
        state->expected = ::unexpected<std::exception_ptr>(std::move(exc)); 
        state->valid = true;
        invoke_Callback();
        state->cond_var.notify_one();
    }
    // * sync with future constr and self constr
    future<T> get_future(){
        return future<T>(state);
    }
    ~promise(){ // * async only read operation
        if(not state){return;}
        std::unique_lock lock(state->mutex);
        if(not state->valid){
            state->expected = ::unexpected<std::exception_ptr>(std::make_exception_ptr(std::logic_error("promise value or exception not setted")));
            state->valid = true;
            // ? notify only if future alive
            state->cond_var.notify_one();
        }
        lock.unlock();
        if(1 == state->alive.exchange(1,std::memory_order_relaxed)){
            delete state;
            return;
        }
    }
};
}

#include <coroutine>

template <cvk::FutureValue T, typename... Args>
struct std::coroutine_traits<cvk::future<T>, Args...>
{
    struct promise_type : cvk::promise<T>{
        cvk::future<T> get_return_object() noexcept
        {
            return this->get_future();
        }
 
        std::suspend_never initial_suspend() const noexcept { return {}; }
        std::suspend_never final_suspend() const noexcept { return {}; }

            // ? only move return value
        void return_value(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            this->set_value(std::move(value));
        }
 
        void unhandled_exception() noexcept
        {
            this->set_exception(std::current_exception());
        }
    };
};

template <cvk::FutureValue T>
struct FutureAwaiter{

    template<class io_context, class F, typename... Args>
requires std::is_invocable_r<cvk::future<T>, F, Args...>::value
    and (std::is_same_v<asio::io_context,std::remove_pointer_t<io_context>>
        or std::is_same_v<asio::io_context, std::remove_reference_t<io_context>>
    )
    FutureAwaiter(io_context& context, F&& func, Args... args)
    :future(func(std::forward<Args>(args)...))
    {
        if constexpr(std::is_same_v<asio::io_context, std::remove_reference_t<io_context>>){
            this->context = &context;
        }else{
            this->context = context;
        }
    }
    bool await_ready()noexcept{return false;}
    void await_suspend(std::coroutine_handle<>cont){
        future.subscribe([this,cont](expected<T,std::exception_ptr>&& expected){
            result = std::move(expected);
            cont();
        }, *context);
    }
    T await_resume(){
        if(result.has_value()){
            return std::move(result.value());
        }
        std::rethrow_exception(result.error());
    }

private:
    cvk::future<T> future;
    expected<T,std::exception_ptr> result;
    asio::io_context* context = nullptr;
};