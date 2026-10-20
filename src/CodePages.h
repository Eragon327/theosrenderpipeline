#pragma once

#include <Windows.h>
#include <cstdint>
#include <xbyak/xbyak.h>

namespace TheosRenderPipeline
{
    // Xbyak's default allocator places a small generator on the heap and marks
    // the whole page executable, so unrelated heap allocations on that page
    // become executable too. Give each generator pages of its own instead.
    class CodePages final : public Xbyak::Allocator
    {
    public:
        std::uint8_t* alloc(std::size_t size) override
        {
            void* pages = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!pages) { throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC); }
            return static_cast<std::uint8_t*>(pages);
        }
        void free(std::uint8_t* pages) override
        {
            if (pages) { VirtualFree(pages, 0, MEM_RELEASE); }
        }
    };

    inline CodePages& CodePageAllocator()
    {
        static CodePages allocator;
        return allocator;
    }
}
