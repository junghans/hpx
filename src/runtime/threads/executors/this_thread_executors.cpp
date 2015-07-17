//  Copyright (c) 2007-2015 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/config.hpp>

#if defined(HPX_HAVE_LOCAL_SCHEDULER) || defined(HPX_HAVE_STATIC_PRIORITY_SCHEDULER)

#include <hpx/runtime/threads/resource_manager.hpp>
#if defined(HPX_HAVE_LOCAL_SCHEDULER)
#include <hpx/runtime/threads/policies/local_queue_scheduler.hpp>
#endif
#if defined(HPX_HAVE_STATIC_PRIORITY_SCHEDULER)
#include <hpx/runtime/threads/policies/static_priority_queue_scheduler.hpp>
#endif
#include <hpx/runtime/threads/detail/scheduling_loop.hpp>
#include <hpx/runtime/threads/detail/create_thread.hpp>
#include <hpx/runtime/threads/detail/set_thread_state.hpp>
#include <hpx/runtime/threads/executors/this_thread_executors.hpp>
#include <hpx/runtime/threads/executors/manage_thread_executor.hpp>
#include <hpx/util/assert.hpp>
#include <hpx/util/bind.hpp>

namespace hpx { namespace threads { namespace detail
{
    // The function \a set_self_ptr sets a pointer to the (OS thread
    // specific) self reference to the current HPX thread.
    void set_self_ptr(threads::thread_self*);
}}}

namespace hpx { namespace threads { namespace executors { namespace detail
{
    ///////////////////////////////////////////////////////////////////////////
    template <typename Scheduler>
    this_thread_executor<Scheduler>::this_thread_executor()
      : scheduler_(1, false), shutdown_sem_(0),
        state_(state_initialized), thread_num_(std::size_t(-1)),
        tasks_scheduled_(0), tasks_completed_(0), cookie_(0),
        self_(0)
    {
        // Inform the resource manager about this new executor. This causes the
        // resource manager to interact with this executor using the
        // manage_executor interface.
        resource_manager& rm = resource_manager::get();
        cookie_ = rm.initial_allocation(
            new manage_thread_executor<this_thread_executor>(*this));
    }

    template <typename Scheduler>
    this_thread_executor<Scheduler>::~this_thread_executor()
    {
        // if we're still starting up, give this executor a chance of executing
        // its tasks
        while (state_.load() < state_running)
            this_thread::suspend();

        // Inform the resource manager that this executor is about to be
        // destroyed. This will cause it to invoke remove_processing_unit below
        // for each of the currently allocated virtual cores.
        resource_manager& rm = resource_manager::get();
        rm.stop_executor(cookie_);

        // wait for executor to finish executing
        shutdown_sem_.wait(1);

        // detach this executor from resource manager
        rm.detach(cookie_);     // this releases proxy (manage_thread_executor)

#if defined(HPX_DEBUG)
        // all resources should have been stopped at this point (or have never
        // been initialized)
        hpx::state s = state_.load();
        HPX_ASSERT(s == state_initialized || s == state_stopped);

        // all scheduled tasks should have completed executing
        HPX_ASSERT(tasks_completed_ == tasks_scheduled_);
#endif
    }

    template <typename Scheduler>
    threads::thread_state_enum
    this_thread_executor<Scheduler>::thread_function_nullary(
        closure_type func)
    {
        // execute the actual thread function
        func();

        // update statistics
        ++tasks_completed_;

        // Verify that there are no more registered locks for this
        // OS-thread. This will throw if there are still any locks
        // held.
        util::force_error_on_lock();

        return threads::terminated;
    }

    // Schedule the specified function for execution in this executor.
    // Depending on the subclass implementation, this may block in some
    // situations.
    template <typename Scheduler>
    void this_thread_executor<Scheduler>::add(closure_type && f,
        char const* desc, threads::thread_state_enum initial_state,
        bool run_now, threads::thread_stacksize stacksize, error_code& ec)
    {
        HPX_ASSERT(std::size_t(-1) != thread_num_);

        // if the scheduler was stopped, we need to restart it
        state expected = state_stopped;
        state_.compare_exchange_strong(expected, state_starting);

        // create a new thread
        thread_init_data data(util::bind(
            util::one_shot(&this_thread_executor::thread_function_nullary),
            this, std::move(f)), desc);
        data.stacksize = threads::get_stack_size(stacksize);

        // update statistics
        ++tasks_scheduled_;

        threads::thread_id_type id = threads::invalid_thread_id;
        threads::detail::create_thread(&scheduler_, data, id, initial_state, //-V601
            run_now, ec);
        if (ec) {
            --tasks_scheduled_;
            return;
        }

        // execute scheduler directly, if necessary
        run(0, thread_num_);

        if (&ec != &throws)
            ec = make_success_code();
    }

    // Schedule given function for execution in this executor no sooner
    // than time abs_time. This call never blocks, and may violate
    // bounds on the executor's queue size.
    template <typename Scheduler>
    void this_thread_executor<Scheduler>::add_at(
        boost::chrono::steady_clock::time_point const& abs_time,
        closure_type && f, char const* desc,
        threads::thread_stacksize stacksize, error_code& ec)
    {
        HPX_ASSERT(std::size_t(-1) != thread_num_);

        // if the scheduler was stopped, we need to restart it
        state expected = state_stopped;
        state_.compare_exchange_strong(expected, state_starting);

        // create a new suspended thread
        thread_init_data data(util::bind(
            util::one_shot(&this_thread_executor::thread_function_nullary),
            this, std::move(f)), desc);
        data.stacksize = threads::get_stack_size(stacksize);

        threads::thread_id_type id = threads::invalid_thread_id;
        threads::detail::create_thread( //-V601
            &scheduler_, data, id, suspended, true, ec);
        if (ec) return;
        HPX_ASSERT(invalid_thread_id != id);    // would throw otherwise

        // update statistics
        ++tasks_scheduled_;

        // now schedule new thread for execution
        threads::detail::set_thread_state_timed(scheduler_, abs_time, id, ec);
        if (ec) {
            --tasks_scheduled_;
            return;
        }

        // execute scheduler directly, if necessary
        run(0, thread_num_);

        if (&ec != &throws)
            ec = make_success_code();
    }

    // Schedule given function for execution in this executor no sooner
    // than time rel_time from now. This call never blocks, and may
    // violate bounds on the executor's queue size.
    template <typename Scheduler>
    void this_thread_executor<Scheduler>::add_after(
        boost::chrono::steady_clock::duration const& rel_time,
        closure_type && f, char const* desc,
        threads::thread_stacksize stacksize, error_code& ec)
    {
        return add_at(boost::chrono::steady_clock::now() + rel_time,
            std::move(f), desc, stacksize, ec);
    }

    // Return an estimate of the number of waiting tasks.
    template <typename Scheduler>
    boost::uint64_t this_thread_executor<Scheduler>::num_pending_closures(
        error_code& ec) const
    {
        if (&ec != &throws)
            ec = make_success_code();
        return (state_.load() < state_stopped) ? 1 : 0;
    }

    ///////////////////////////////////////////////////////////////////////////
    struct on_self_reset
    {
        on_self_reset(threads::thread_self* self)
        {
            threads::detail::set_self_ptr(self);
        }
        ~on_self_reset()
        {
            threads::detail::set_self_ptr(0);
        }
    };

    template <typename Scheduler>
    void this_thread_executor<Scheduler>::suspend_back_into_calling_context(
        std::size_t virt_core)
    {
        HPX_ASSERT(0 == virt_core);

        // Give invoking context a chance to catch up with its tasks, but only
        // if this scheduler is currently in running state (this scheduler is
        // always in stopping state as it has to exit as early as possible).
        state expected = state_stopping;
        if (state_.compare_exchange_strong(expected, state_suspended))
        {
            {
                on_self_reset on_exit(self_);
                this_thread::suspend();
            }

            // reset state to running if current state is still suspended
            expected = state_suspended;
            state_.compare_exchange_strong(expected, state_stopping);
        }
        else
        {
            HPX_ASSERT(expected != state_suspended);
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    struct on_run_exit
    {
        on_run_exit(lcos::local::counting_semaphore& shutdown_sem,
                threads::thread_self* self)
          : shutdown_sem_(shutdown_sem),
            self_(self)
        {
            threads::detail::set_self_ptr(0);
        }

        ~on_run_exit()
        {
            threads::detail::set_self_ptr(self_);
            shutdown_sem_.signal();
        }

        lcos::local::counting_semaphore& shutdown_sem_;
        threads::thread_self* self_;
    };

    // execute all work
    template <typename Scheduler>
    void this_thread_executor<Scheduler>::run(std::size_t virt_core,
        std::size_t thread_num)
    {
        HPX_ASSERT(0 == virt_core);

        // We want to exit this scheduling loop as early as possible, thus
        // we use 'state_stopping' instead of 'state_running'.

        // Set the state to 'state_stopping' only if it's still in
        // 'state_starting' state, otherwise our destructor is currently being
        // executed, which means we need to still execute all threads.
        state expected = state_starting;
        if (state_.compare_exchange_strong(expected, state_stopping))
        {
            {
                typename mutex_type::scoped_lock l(mtx_);
                scheduler_.add_punit(virt_core, thread_num);
                scheduler_.on_start_thread(virt_core);
            }

            self_ = threads::get_self_ptr();

            on_run_exit on_exit(shutdown_sem_, self_);

            // FIXME: turn these values into performance counters
            boost::int64_t executed_threads = 0, executed_thread_phases = 0;
            boost::uint64_t overall_times = 0, thread_times = 0;

            threads::detail::scheduling_counters counters(
                executed_threads, executed_thread_phases,
                overall_times, thread_times);

            threads::detail::scheduling_loop(virt_core, scheduler_,
                state_, counters, util::function_nonser<void()>(),
                util::bind( //-V107
                    &this_thread_executor::suspend_back_into_calling_context,
                    this, virt_core));

            // the scheduling_loop is allowed to exit only if no more HPX
            // threads exist
            HPX_ASSERT(!scheduler_.get_thread_count(
                unknown, thread_priority_default, virt_core) ||
                state_ == state_terminating);
        }
    }

    // Return statistics collected by this scheduler
    template <typename Scheduler>
    void this_thread_executor<Scheduler>::get_statistics(
        executor_statistics& stats, error_code& ec) const
    {
        if (&ec != &throws)
            ec = make_success_code();

        stats.queue_length_ = scheduler_.get_queue_length();
        stats.tasks_scheduled_ = tasks_scheduled_.load();
        stats.tasks_completed_ = tasks_completed_.load();
    }

    // Return the requested policy element
    template <typename Scheduler>
    std::size_t this_thread_executor<Scheduler>::get_policy_element(
        threads::detail::executor_parameter p, error_code& ec) const
    {
        switch(p) {
        case threads::detail::min_concurrency:
        case threads::detail::max_concurrency:
        case threads::detail::current_concurrency:
            return 1;

        default:
            break;
        }

        HPX_THROWS_IF(ec, bad_parameter,
            "this_thread_executor::get_policy_element",
            "requested value of invalid policy element");
        return std::size_t(-1);
    }

    // Provide the given processing unit to the scheduler.
    template <typename Scheduler>
    void this_thread_executor<Scheduler>::add_processing_unit(
        std::size_t virt_core, std::size_t thread_num, error_code& ec)
    {
        HPX_ASSERT(0 == virt_core);
        HPX_ASSERT(std::size_t(-1) == thread_num_);

        thread_num_ = thread_num;

        state expected = state_initialized;
        bool result = state_.compare_exchange_strong(expected, state_starting);
        HPX_ASSERT(result);
    }

    // Remove the given processing unit from the scheduler.
    template <typename Scheduler>
    void this_thread_executor<Scheduler>::remove_processing_unit(
        std::size_t virt_core, error_code& ec)
    {
        HPX_ASSERT(0 == virt_core);
        HPX_ASSERT(std::size_t(-1) != thread_num_);

        // inform the scheduler to stop the virtual core
        state oldstate = state_.exchange(state_stopped);
        HPX_ASSERT(oldstate == state_suspended || oldstate == state_stopped);

        thread_num_ = std::size_t(-1);
    }
}}}}

#endif

namespace hpx { namespace threads { namespace executors
{
#if defined(HPX_HAVE_LOCAL_SCHEDULER)
    ///////////////////////////////////////////////////////////////////////////
    this_thread_local_queue_executor::this_thread_local_queue_executor()
      : scheduled_executor(new detail::this_thread_executor<
            policies::local_queue_scheduler<> >())
    {}
#endif

#if defined(HPX_HAVE_STATIC_PRIORITY_SCHEDULER)
    ///////////////////////////////////////////////////////////////////////////
    this_thread_static_priority_queue_executor::
            this_thread_static_priority_queue_executor()
      : scheduled_executor(new detail::this_thread_executor<
            policies::static_priority_queue_scheduler<> >())
    {}
#endif
}}}
