// Small stand-ins for the MAME framework types the ported Model 1 files use.
#pragma once
#include "../core/mame_shim.h"
#include <algorithm>
#include <memory>
#include <vector>
#include <cmath>
#include <deque>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

struct rectangle
{
	int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	rectangle() = default;
	rectangle(int x0, int x1, int y0, int y1) : min_x(x0), max_x(x1), min_y(y0), max_y(y1) {}
	int width() const { return max_x - min_x + 1; }
	int height() const { return max_y - min_y + 1; }
};

// A bitmap's pixels page-aligned and a whole number of pages long: the GPU's 3D (src/gpu) draws into
// them where they are (a Metal buffer made on the same memory), nothing copied either way.
template <class T> struct page_allocator
{
	using value_type = T;
	static constexpr size_t PAGE = 16384;
	page_allocator() = default;
	template <class U> page_allocator(const page_allocator<U> &) {}
	static size_t bytes(size_t n) { return (n * sizeof(T) + PAGE - 1) / PAGE * PAGE; }
	T *allocate(size_t n)
	{
#ifdef _WIN32
		void *p = _aligned_malloc(std::max<size_t>(bytes(n), PAGE), PAGE);
#else
		void *p = std::aligned_alloc(PAGE, std::max<size_t>(bytes(n), PAGE));
#endif
		if (!p) throw std::bad_alloc();
		return static_cast<T *>(p);
	}
#ifdef _WIN32
	void deallocate(T *p, size_t) { _aligned_free(p); }
#else
	void deallocate(T *p, size_t) { std::free(p); }
#endif
	template <class U> bool operator==(const page_allocator<U> &) const { return true; }
	template <class U> bool operator!=(const page_allocator<U> &) const { return false; }
};

class bitmap_rgb32
{
public:
	void allocate(int w, int h) { m_w = w; m_h = h; m_pix.assign(size_t(w) * h, 0); }
	uint32_t &pix(int y, int x = 0) { return m_pix[size_t(y) * m_w + x]; }
	const uint32_t &pix(int y, int x = 0) const { return m_pix[size_t(y) * m_w + x]; }
	int width() const { return m_w; }
	int height() const { return m_h; }
	rectangle cliprect() const { return rectangle(0, m_w - 1, 0, m_h - 1); }
	void fill(uint32_t c, const rectangle &r)
	{
		for (int y = r.min_y; y <= r.max_y; y++)
			std::fill(&pix(y, r.min_x), &pix(y, r.max_x) + 1, c);
	}
	void fill(uint32_t c) { std::fill(m_pix.begin(), m_pix.end(), c); }
	const uint32_t *data() const { return m_pix.data(); }
	uint32_t *data() { return m_pix.data(); }
	size_t bytes() const { return page_allocator<uint32_t>::bytes(m_pix.size()); }   // (its pages')
private:
	int m_w = 0, m_h = 0;
	std::vector<uint32_t, page_allocator<uint32_t>> m_pix;
};

constexpr uint8_t pal5bit(uint8_t b) { b &= 0x1f; return (b << 3) | (b >> 2); }
template <typename T> std::unique_ptr<T> make_unique_clear(size_t n)
{
	using E = std::remove_extent_t<T>;
	auto p = std::make_unique<E[]>(n);
	std::fill(p.get(), p.get() + n, E{});
	return p;
}
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// 16-entry-deep FIFO with MAME's generic_fifo calls.
class u32_fifo
{
public:
	void push(uint32_t v) { m_q.push_back(v); if (m_on_push) m_on_push(); }
	uint32_t pop() { if (m_q.empty()) { m_underflow = true; return 0; } uint32_t v = m_q.front(); m_q.pop_front(); return v; }
	uint32_t peek(int i) const { return m_q[i]; }
	size_t size() const { return m_q.size(); }
	bool is_empty() const { return m_q.empty(); }
	void clear() { m_q.clear(); }
	std::function<void()> m_on_push;
	bool m_underflow = false;
	std::deque<uint32_t> &queue() { return m_q; }   // (the save state)
private:
	std::deque<uint32_t> m_q;
};

#define M1_LOG(...) ((void)0)
#define COMBINE_DATA(varptr) (*(varptr) = (*(varptr) & ~mem_mask) | (data & mem_mask))
