#ifndef TIME_TRACKER_HPP_
#define TIME_TRACKER_HPP_

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <sstream>
#include <ostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <opencv2/core/cvdef.h>

using std::ostream;
using std::stringstream;
using std::string;
using std::map;
using std::chrono::microseconds;
using std::mutex;

/*!
 * Timings of one tracked section. All durations are microseconds.
 *
 * Two averages are kept: #avgTotal() covers every call since the statistics
 * were created (or last reset), #avgIter() covers only the calls of the current
 * counting window, i.e. the frame that is being displayed right now. The former
 * is stable, the latter reacts to what the pipeline is doing *now* - the
 * on-screen widget (#TimeTrackerWidget) shows both.
 */
struct CV_EXPORTS TimeInfo {
    long totalCnt_ = 0;
    long totalTime_ = 0;
    long iterCnt_ = 0;
    long iterTime_ = 0;
    long last_ = 0;
    // Slowest single measurement since the last reset, i.e. the worst jitter
    // this section has shown so far.
    long max_ = 0;

    void add(size_t t) {
        last_ = static_cast<long>(t);
        if(last_ > max_)
            max_ = last_;
        totalTime_ += last_;
        iterTime_ += last_;
        ++totalCnt_;
        ++iterCnt_;

        if (totalCnt_ == std::numeric_limits<long>::max() || totalTime_ == std::numeric_limits<long>::max()) {
            totalCnt_ = 0;
            totalTime_ = 0;
            max_ = 0;
        }

        if (iterCnt_ == std::numeric_limits<long>::max() || iterTime_ == std::numeric_limits<long>::max()) {
            iterCnt_ = 0;
            iterTime_ = 0;
        }
    }

    /*! Folds the calls of the finished counting window into a single average. */
    void newCount() {
    	if(iterCnt_ > 0)
    		iterTime_ = iterTime_ / iterCnt_;
        iterCnt_ = 1;
    }

    /*! Average duration of one call since the last reset, in microseconds. */
    double avgTotal() const {
        return totalCnt_ > 0 ? (double)totalTime_ / (double)totalCnt_ : 0.0;
    }

    /*! Average duration of one call in the current counting window, in microseconds. */
    double avgIter() const {
        return iterCnt_ > 0 ? (double)iterTime_ / (double)iterCnt_ : 0.0;
    }

    string str() const {
        stringstream ss;
        ss << (avgTotal() / 1000.0) << "ms / ";
        ss << (avgIter() / 1000.0) << "ms";
        return ss.str();
    }
};

inline std::ostream& operator<<(ostream &os, const TimeInfo &ti) {
    os << std::fixed << std::setprecision(8) << std::setfill(' ') << std::setw(13) << (ti.avgTotal() / 1000.0) << "ms / ";
    os << std::setfill(' ') << std::setw(13) << (ti.avgIter() / 1000.0) << "ms" << std::scientific;
    return os;
}

struct TimeSortCompare {
	inline bool operator()(const TimeInfo &lhs, const TimeInfo &rhs) const {
		return lhs.avgTotal() > rhs.avgTotal();
	}
};

class CV_EXPORTS TimeTracker {
private:
    static TimeTracker *instance_;
    // Tracker of the plan running on the calling thread (if any). Two plans
    // running at the same time must not report into each other's statistics,
    // so each run gets its own tracker while code outside of a run (e.g. an
    // after-run summary in main) keeps using the process-wide instance.
    static thread_local TimeTracker *threadInstance_;
    mutable mutex mapMtx_;
    map<string, TimeInfo> tiMap_;
    std::atomic<bool> enabled_;
    TimeTracker();
public:
    virtual ~TimeTracker();

    /*!
     * The live map. It is written by every worker thread of the run, so
     * consumers that are not a worker (the GUI widget, #print) must take a
     * #snapshot() instead of reading it.
     */
    map<string, TimeInfo>& getMap() {
        return tiMap_;
    }

    /*!
     * A consistent copy of the current statistics. Prefer this over #getMap()
     * whenever the numbers are shown to someone: the copy is taken under the
     * lock, so it never observes a half-written entry.
     */
    CV_EXPORTS std::vector<std::pair<string, TimeInfo>> snapshot() const;

    template<typename F> void execute(const string &name, F const &func) {
        auto start = std::chrono::steady_clock::now();
        func();
        auto duration = std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now() - start);
        std::unique_lock lock(mapMtx_);
        tiMap_[name].add(duration.count());
    }

    template<typename F> size_t measure(F const &func) {
        auto start = std::chrono::steady_clock::now();
        func();
        auto duration = std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now() - start);
        return duration.count();
    }

    bool isEnabled() const {
        return enabled_.load(std::memory_order_relaxed);
    }

    void setEnabled(bool e) {
        enabled_.store(e, std::memory_order_relaxed);
    }

    void print(ostream &os) {
        if(!isEnabled())
            return;

        std::unique_lock lock(mapMtx_);
        stringstream ss;
        ss << "Time tracking info: " << std::endl;
        map<TimeInfo, string, TimeSortCompare> timeSortedMap;
        for (auto pair : tiMap_) {
        	timeSortedMap.insert({pair.second, pair.first});
        }

        for (auto pair : timeSortedMap) {
        	ss << pair.first << "\t" << pair.second << " " << std::endl;
        }

        os << ss.str();
    }

    void reset() {
        if(!isEnabled())
            return;

        std::unique_lock lock(mapMtx_);
        tiMap_.clear();
    }

    static TimeTracker* getInstance() {
        if (threadInstance_ != NULL)
            return threadInstance_;

        if (instance_ == NULL)
            instance_ = new TimeTracker();

        return instance_;
    }

    /*!
     * Binds a tracker to the calling thread, i.e. to the plan that thread
     * takes part in. Pass nullptr to detach it again.
     */
    static void setThreadInstance(TimeTracker* tracker) {
        threadInstance_ = tracker;
    }

    /*!
     * A tracker of its own, for a plan that needs its statistics separate from
     * the process-wide tracker.
     */
    static std::shared_ptr<TimeTracker> create() {
        return std::shared_ptr<TimeTracker>(new TimeTracker());
    }

    static void destroy() {
        if (instance_)
            delete instance_;

        instance_ = NULL;
        threadInstance_ = NULL;
    }

    /*!
     * Folds the calls of the frame that just went by into their average, so the
     * next frame reports the per-frame cost of a section rather than the sum of
     * everything it did during that frame.
     */
    CV_EXPORTS void newCount() {
        if(!isEnabled())
            return;
        std::unique_lock lock(mapMtx_);
        for (auto& pair : tiMap_) {
            pair.second.newCount();
        }
    }
};

#endif /* TIME_TRACKER_HPP_ */
