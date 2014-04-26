#include "asynclog/detail/log_base.hpp"
#include "asynclog/detail/frame.hpp"

asynclog::detail::log_base::log_base(writer* pwriter, 
        std::size_t input_frame_alignment,
        std::size_t output_buffer_max_capacity,
        std::size_t shared_input_queue_size,
        std::size_t thread_input_buffer_size) :
    shared_input_queue_(shared_input_queue_size),
    pthread_input_buffer_(this, thread_input_buffer_size, input_frame_alignment),
    output_buffer_(pwriter, output_buffer_max_capacity),
    output_thread_(std::mem_fn(&log_base::output_worker), this)
{
}

asynclog::detail::log_base::~log_base()
{
    using namespace detail;
    commit();
    queue_commit_extent({nullptr, 0});
    output_thread_.join();
    assert(shared_input_queue_.empty());
}

void asynclog::detail::log_base::commit()
{
    using namespace detail;
    thread_input_buffer* pib = pthread_input_buffer_.get();
    auto pcommit_end = pib->input_end();
    queue_commit_extent({pib, pcommit_end});
}

void asynclog::detail::log_base::output_worker()
{
    using namespace detail;
    while(true) {
        commit_extent ce;
        unsigned wait_time_ms = 0;
        while(not shared_input_queue_.pop(ce)) {
            shared_input_queue_full_event_.wait(wait_time_ms);
            wait_time_ms = (wait_time_ms == 0)? 1 : 2*wait_time_ms;
            wait_time_ms = std::min(wait_time_ms, 1000u);
        }
        shared_input_consumed_event_.signal();
            
        if(not ce.pinput_buffer)
            // Request to shut down thread.
            return;

        char* pinput_start = ce.pinput_buffer->input_start();
        while(pinput_start != ce.pcommit_end) {
            auto pdispatch = *reinterpret_cast<dispatch_function_t**>(pinput_start);
            if(WRAPAROUND_MARKER == pdispatch) {
                pinput_start = ce.pinput_buffer->wraparound();
                pdispatch = *reinterpret_cast<dispatch_function_t**>(pinput_start);
            }
            auto frame_size = (*pdispatch)(&output_buffer_, pinput_start);
            pinput_start = ce.pinput_buffer->discard_input_frame(frame_size);
        }
        // TODO we *could* do something like flush on a timer instead when we're getting a lot of writes / sec.
        // OR, we should at least keep on dumping data without flush as long as the input queue has data to give us.
        output_buffer_.flush();
    }
}

void asynclog::detail::log_base::queue_commit_extent(detail::commit_extent const& ce)
{
    using namespace detail;
    if(unlikely(not shared_input_queue_.push(ce))) {
        do {
            shared_input_queue_full_event_.signal();
            shared_input_consumed_event_.wait();
        } while(not shared_input_queue_.push(ce));
    }
}