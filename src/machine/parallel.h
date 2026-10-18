// A small pool of worker threads for splitting a frame's drawing by rows.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class row_pool
{
public:
	static row_pool &get() { static row_pool p; return p; }
	int threads() const { return int(m_workers.size()) + 1; }

	// Calls f(begin, end) over [0, n) in per_thread bands for each thread, and waits. With more
	// than one band a thread, each thread takes the next band when it's done with its last, so a
	// thread with heavy rows (the floor) doesn't leave the rest waiting.
	// few: as few as 2 parts are worth sharing out (each part is a big job of its own)
	void run(int n, const std::function<void(int, int)> &f, int per_thread = 1, bool few = false)
	{
		const int t = threads();
		// inside a band (a worker's, or this thread's mid-run): no pool to hand it to, so here
		if (n < (few ? 2 : 2 * t) || t == 1 || t_in_pool) { f(0, n); return; }
		// one caller at a time (another thread drawing at once would share the counters)
		std::lock_guard<std::mutex> one(m_run_mu);
		struct in_pool { in_pool() { t_in_pool = true; } ~in_pool() { t_in_pool = false; } } mark;
		{
			std::lock_guard<std::mutex> l(m_mu);
			m_job = &f;
			m_n = n;
			m_parts = std::min(n, t * std::max(1, per_thread));
			m_next = 0;
			m_left = t - 1;             // the workers still to finish
			m_gen++;
		}
		m_cv.notify_all();
		take_bands(f, n);
		std::unique_lock<std::mutex> l(m_mu);
		m_done.wait(l, [&] { return m_left == 0; });
		m_job = nullptr;
	}

private:
	std::vector<std::thread> m_workers;
	std::mutex m_mu, m_run_mu;
	static inline thread_local bool t_in_pool = false;
	std::condition_variable m_cv, m_done;
	const std::function<void(int, int)> *m_job = nullptr;
	int m_n = 0, m_parts = 0, m_next = 0, m_left = 0;
	unsigned m_gen = 0;
	bool m_quit = false;

	static std::pair<int, int> band(int i, int n, int parts) { return { n * i / parts, n * (i + 1) / parts }; }

	// draws bands until none are left
	void take_bands(const std::function<void(int, int)> &f, int n)
	{
		for (;;) {
			int i, parts;
			{
				std::lock_guard<std::mutex> l(m_mu);
				if (m_next >= m_parts) return;
				i = m_next++;
				parts = m_parts;
			}
			auto [b, e] = band(i, n, parts);
			f(b, e);
		}
	}

	row_pool()
	{
		int n = std::clamp(int(std::thread::hardware_concurrency()) - 2, 0, 7);   // the CPU and sound keep theirs
		if (const char *t = getenv("M1_THREADS")) n = std::clamp(atoi(t) - 1, 0, 63);   // testing: this many drawing threads in all
		for (int i = 0; i < n; i++)
			m_workers.emplace_back([this] {
				unsigned seen = 0;
				for (;;) {
					std::unique_lock<std::mutex> l(m_mu);
					m_cv.wait(l, [&] { return m_quit || m_gen != seen; });
					if (m_quit) return;
					seen = m_gen;
					const auto *job = m_job;
					const int n = m_n;
					l.unlock();
					t_in_pool = true;
					take_bands(*job, n);
					t_in_pool = false;
					l.lock();
					if (--m_left == 0) m_done.notify_one();
				}
			});
	}
	~row_pool()
	{
		{ std::lock_guard<std::mutex> l(m_mu); m_quit = true; }
		m_cv.notify_all();
		for (auto &w : m_workers) w.join();
	}
};

inline void parallel_rows(int n, const std::function<void(int, int)> &f, int per_thread = 1) { row_pool::get().run(n, f, per_thread); }
// n big jobs (n may be smaller than the threads), one at a time to each thread that's free
inline void parallel_jobs(int n, const std::function<void(int)> &f) { row_pool::get().run(n, [&](int a, int b) { for (int i = a; i < b; i++) f(i); }, 1, true); }
