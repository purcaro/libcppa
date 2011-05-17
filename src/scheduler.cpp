#include <atomic>

#define _GLIBCXX_HAS_GTHREADS
#include <mutex>

#include "cppa/scheduler.hpp"
#include "cppa/detail/mock_scheduler.hpp"

namespace {

std::mutex m_instance_mtx;
std::atomic<cppa::scheduler*> m_instance;

}

namespace cppa {

scheduler::~scheduler() { }

scheduler* get_scheduler()
{
	scheduler* result = m_instance.load();
	if (!result)
	{
		std::lock_guard<std::mutex> lock(m_instance_mtx);
		result = m_instance.load();
		if (!result)
		{
			result = new detail::mock_scheduler;
			m_instance.store(result);
		}
	}
	return result;
}

} // namespace cppa