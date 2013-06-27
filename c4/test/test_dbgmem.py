from support import *
import random


def test_clear_large_memory_chunk():
    PAGE_SIZE = 4096
    blocksize = 8 * PAGE_SIZE
    block = ffi.new("char[]", blocksize)
    blockbase = int(ffi.cast("intptr_t", block))

    for i in range(1000):

        if random.random() < 0.25:
            base = PAGE_SIZE - (blockbase & (PAGE_SIZE-1))
            assert ((blockbase + base) & (PAGE_SIZE-1)) == 0
        else:
            base = random.randrange(1, PAGE_SIZE+1)
        assert base > 0

        length = random.randrange(1, 6 * PAGE_SIZE)
        if random.random() < 0.25:
            blockend = blockbase + base + length
            blockend &= ~(PAGE_SIZE-1)
            length = blockend - (blockbase + base)
            if length <= 0:
                continue

        assert length > 0
        assert base + length + 1 < blocksize

        already_cleared = random.randrange(0, length+1)
        already_cleared &= ((1 << random.randrange(0, 16)) - 1)
        assert 0 <= already_cleared <= length

        block[base - 1] = '<'
        lib.memset(block + base, ord('.'), length - already_cleared)
        lib.memset(block + base + length - already_cleared, 0, already_cleared)
        block[base + length] = '>'

        print hex(blockbase + base), hex(length), hex(already_cleared)
        lib.stm_clear_large_memory_chunk(block + base, length, already_cleared)

        assert block[base - 1] == '<'
        assert block[base + length] == '>'
        assert ffi.buffer(block + base, length)[:] == '\x00' * length
