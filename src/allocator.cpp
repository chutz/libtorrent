/*

Copyright (c) 2009-2016, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/allocator.hpp"
#include "libtorrent/config.hpp"
#include "libtorrent/assert.hpp" // for print_backtrace
#include <cstdint>

#if defined TORRENT_BEOS
#include <kernel/OS.h>
#include <stdlib.h> // malloc/free
#elif !defined TORRENT_WINDOWS
#include <cstdlib> // posix_memalign/free
#include <unistd.h> // _SC_PAGESIZE
#endif

#if TORRENT_USE_MEMALIGN || TORRENT_USE_POSIX_MEMALIGN || defined TORRENT_WINDOWS
#include <malloc.h> // memalign and _aligned_malloc
#include <stdlib.h> // _aligned_malloc on mingw
#endif

#ifdef TORRENT_WINDOWS
// windows.h must be included after stdlib.h under mingw
#include <windows.h>
#endif

#ifdef TORRENT_MINGW
#define _aligned_malloc __mingw_aligned_malloc
#define _aligned_free __mingw_aligned_free
#endif

#ifdef TORRENT_DEBUG_BUFFERS
#ifndef TORRENT_WINDOWS
#include <sys/mman.h>
#endif

struct alloc_header
{
	std::int64_t size;
	int magic;
	char stack[3072];
};

#endif

namespace libtorrent
{

	int page_size()
	{
		static int s = 0;
		if (s != 0) return s;

#ifdef TORRENT_BUILD_SIMULATOR
		s = 4096;
#elif defined TORRENT_WINDOWS
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		s = si.dwPageSize;
#elif defined TORRENT_BEOS
		s = B_PAGE_SIZE;
#else
		s = int(sysconf(_SC_PAGESIZE));
#endif
		// assume the page size is 4 kiB if we
		// fail to query it
		if (s <= 0) s = 4096;
		return s;
	}

	char* page_aligned_allocator::malloc(page_aligned_allocator::size_type bytes)
	{
		TORRENT_ASSERT(bytes > 0);
		// just sanity check (this needs to be pretty high
		// for cases where the cache size is several gigabytes)
		TORRENT_ASSERT(bytes < 0x30000000);

		TORRENT_ASSERT(int(bytes) >= page_size());
#ifdef TORRENT_DEBUG_BUFFERS
		const int page = page_size();
		const int num_pages = (bytes + (page - 1)) / page + 2;
		const int orig_bytes = bytes;
		bytes = num_pages * page;
#endif

		void* ret;
#if TORRENT_USE_POSIX_MEMALIGN
		if (posix_memalign(&ret, std::size_t(page_size()), std::size_t(bytes))
			!= 0) ret = nullptr;
#elif TORRENT_USE_MEMALIGN
		ret = memalign(std::size_t(page_size()), std::size_t(bytes));
#elif defined TORRENT_WINDOWS
		ret = _aligned_malloc(std::size_t(bytes), std::size_t(page_size()));
#elif defined TORRENT_BEOS
		area_id id = create_area("", &ret, B_ANY_ADDRESS
			, (bytes + page_size() - 1) & (page_size() - 1), B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (id < B_OK) return nullptr;
#else
		ret = valloc(std::size_t(bytes));
#endif
		if (ret == nullptr) return nullptr;

#ifdef TORRENT_DEBUG_BUFFERS
		// make the two surrounding pages non-readable and -writable
		alloc_header* h = static_cast<alloc_header*>(ret);
		h->size = orig_bytes;
		h->magic = 0x1337;
		print_backtrace(h->stack, sizeof(h->stack));

#ifdef TORRENT_WINDOWS
#define mprotect(buf, size, prot) VirtualProtect(buf, size, prot, nullptr)
#define PROT_READ PAGE_READONLY
#endif
		mprotect(ret, std::size_t(page), PROT_READ);
		mprotect(static_cast<char*>(ret) + (num_pages - 1) * page, std::size_t(page), PROT_READ);

#ifdef TORRENT_WINDOWS
#undef mprotect
#undef PROT_READ
#endif
//		std::fprintf(stderr, "malloc: %p head: %p tail: %p size: %d\n", ret + page, ret, ret + page + bytes, int(bytes));

		return static_cast<char*>(ret) + page;
#else
		return static_cast<char*>(ret);
#endif // TORRENT_DEBUG_BUFFERS
	}

	void page_aligned_allocator::free(char* block)
	{
		if (block == nullptr) return;

#ifdef TORRENT_DEBUG_BUFFERS

#ifdef TORRENT_WINDOWS
#define mprotect(buf, size, prot) VirtualProtect(buf, size, prot, nullptr)
#define PROT_READ PAGE_READONLY
#define PROT_WRITE PAGE_READWRITE
#endif
		int const page = page_size();
		// make the two surrounding pages non-readable and -writable
		mprotect(block - page, std::size_t(page), PROT_READ | PROT_WRITE);
		alloc_header* h = reinterpret_cast<alloc_header*>(block - page);
		int const num_pages = int((h->size + (page - 1)) / page + 2);
		TORRENT_ASSERT(h->magic == 0x1337);
		mprotect(block + (num_pages - 2) * page, std::size_t(page), PROT_READ | PROT_WRITE);
//		std::fprintf(stderr, "free: %p head: %p tail: %p size: %d\n", block, block - page, block + h->size, int(h->size));
		h->magic = 0;
		block -= page;

#ifdef TORRENT_WINDOWS
#undef mprotect
#undef PROT_READ
#undef PROT_WRITE
#endif

		print_backtrace(h->stack, sizeof(h->stack));

#endif // TORRENT_DEBUG_BUFFERS

#ifdef TORRENT_WINDOWS
		_aligned_free(block);
#elif defined TORRENT_BEOS
		area_id id = area_for(block);
		if (id < B_OK) return;
		delete_area(id);
#else
		::free(block);
#endif // TORRENT_WINDOWS
	}

#ifdef TORRENT_DEBUG_BUFFERS
	bool page_aligned_allocator::in_use(char const* block)
	{
		const int page = page_size();
		alloc_header const* h = reinterpret_cast<alloc_header const*>(block - page);
		return h->magic == 0x1337;
	}
#endif

}
