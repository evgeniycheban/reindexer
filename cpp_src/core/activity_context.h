#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include "estl/mutex.h"
#include "estl/string_view.h"

namespace reindexer {

class WrSerializer;

struct Activity {
	unsigned id;
	std::string activityTracer;
	std::string user;
	std::string query;
	std::chrono::system_clock::time_point startTime;
	enum State : unsigned { InProgress = 0, WaitLock, Sending, IndexesLookup, SelectLoop } state;
	string_view description;
	void GetJSON(WrSerializer&) const;
	static string_view DescribeState(State);
};

class RdxActivityContext;

class ActivityContainer {
public:
	void Register(const RdxActivityContext*);
	void Unregister(const RdxActivityContext*);
	void Reregister(const RdxActivityContext* oldCtx, const RdxActivityContext* newCtx);
	std::vector<Activity> List();

private:
	std::mutex mtx_;
	std::unordered_set<const RdxActivityContext*> cont_;
};

/// The only threadsafe operation with objects of this class is cast to Activity
class RdxActivityContext {
	constexpr static unsigned kStateShift = 3u;
	constexpr static unsigned kStateMask = (1u << kStateShift) - 1u;
	friend class RdxContext;

	class Ward {
	public:
		Ward(RdxActivityContext* cont, Activity::State state) : context_(cont) {
			if (context_) {
				prevState_ = context_->state_.exchange(serializeState(state), std::memory_order_relaxed);
				context_->refCount_.fetch_add(1u, std::memory_order_relaxed);
			}
		}
		Ward(RdxActivityContext* cont, MutexMark mutexMark) : context_(cont) {
			if (context_) {
				prevState_ = context_->state_.exchange(serializeState(mutexMark), std::memory_order_relaxed);
				context_->refCount_.fetch_add(1u, std::memory_order_relaxed);
			}
		}
		Ward(Ward&& other) : context_(other.context_), prevState_(other.prevState_) { other.context_ = nullptr; }
		~Ward() {
			if (context_) {
				context_->state_.store(prevState_, std::memory_order_relaxed);
				const unsigned prevValue = context_->refCount_.fetch_sub(1u, std::memory_order_relaxed);
				assert(prevValue != 0u);
				(void)prevValue;
			}
		}

		Ward(const Ward&) = delete;
		Ward& operator=(const Ward&) = delete;
		Ward& operator=(Ward&&) = delete;

	private:
		RdxActivityContext* context_;
		unsigned prevState_{serializeState(Activity::InProgress)};
	};

public:
	RdxActivityContext(string_view activityTracer, string_view user, string_view query, ActivityContainer&, bool clientState = false);
	RdxActivityContext(RdxActivityContext&&);
	~RdxActivityContext() {
		if (parent_) {
			parent_->Unregister(this);
			assert(refCount_.load(std::memory_order_relaxed) == 0u);
		}
	}
	operator Activity() const;

	RdxActivityContext(const RdxActivityContext&) = delete;
	RdxActivityContext& operator=(const RdxActivityContext&) = delete;
	RdxActivityContext& operator=(RdxActivityContext&&) = delete;

	/// returning value of these functions should be assined to a local variable which will be destroyed after the waiting work complete
	/// lifetime of the local variable should not exceed of the activityContext's
	Ward BeforeLock(MutexMark mutexMark) { return Ward(this, mutexMark); }
	Ward BeforeIndexWork() { return Ward(this, Activity::IndexesLookup); }
	Ward BeforeSelectLoop() { return Ward(this, Activity::SelectLoop); }

private:
	static unsigned serializeState(MutexMark);
	static unsigned serializeState(Activity::State);
	static std::pair<Activity::State, string_view> deserializeState(unsigned state);
	static unsigned nextId() noexcept;

	const Activity data_;
	std::atomic<unsigned> state_{serializeState(Activity::InProgress)};  // kStateShift lower bits for state, other for details
	ActivityContainer* parent_;
	std::atomic<unsigned> refCount_;
};

}  // namespace reindexer