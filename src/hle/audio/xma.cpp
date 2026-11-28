// XMA decoding contexts of the audio hardware (see apu.h).
//
// The context is a 64-byte record of bit fields, read and written here field by field. Input is a
// pair of buffers of 2 KB packets; each packet starts with a 32-bit header (frames beginning in it,
// bit offset of the first one, metadata, packets of other streams to skip to reach this stream's
// next packet). Frames start with a 15-bit length in bits, run on across packet boundaries (past the
// next packet's header), never across buffers, and decode to 512 samples: four subframes of 128.
// A length of all ones marks the end of the data.
//
// Output is 16-bit samples, interleaved for stereo, into a ring of 256-byte blocks that titles drain
// by moving the ring's read offset. Frames are decoded whole (xma_decoder.cpp) and go out a
// subframe at a time as the ring has room, so titles pace decoding by draining it. Loop points
// name the frames holding the loop end and start and the subframes within them.

#include <windows.h>
#include <string.h>

#include "common/log.h"
#include "hle/audio/apu.h"
#include "hle/audio/xma_decoder.h"

// --- context fields --------------------------------------------------------------------------------

struct Field {
    u8 word;
    u8 shift;
    u8 bits;
};

static const Field SIZE_READ0 = {0, 0, 12}, NUM_LOOPS = {0, 12, 8}, VALID_BUFFER = {0, 20, 2},
                   SIZE_WRITE = {0, 22, 5}, OFFSET_WRITE = {0, 27, 5}, SIZE_READ1 = {1, 0, 12},
                   LOOP_SUBFRAME_END = {1, 12, 2}, LOOP_SUBFRAME_SKIP = {1, 17, 3},
                   SUBFRAMES_TO_DECODE = {1, 20, 4}, SUBFRAMES_TO_SKIP = {1, 24, 3}, SAMPLE_RATE = {1, 27, 2},
                   NUM_CHANNELS = {1, 29, 1}, VALID_WRITE = {1, 31, 1}, OFFSET_READ = {2, 0, 26},
                   LOOP_START = {3, 0, 26}, PARSER_ERROR_STATUS = {3, 26, 5}, PARSER_ERROR_SET = {3, 31, 1},
                   LOOP_END = {4, 0, 26}, PACKET_METADATA = {4, 26, 5}, CURRENT_BUFFER = {4, 31, 1},
                   PTR_READ0 = {5, 0, 32}, PTR_READ1 = {6, 0, 32}, PTR_WRITE = {7, 0, 32},
                   PTR_OVERLAP_ADD = {8, 0, 32}, WRITE_BUFFER_OFFSET_READ = {9, 0, 5};
// Word 2 bits 26..31 hold the decoder's error status (5 bits) and error set flag; decoder errors
// are never raised, so they are not listed.

static u32 get(const u32* c, Field f) {
    u32 mask = f.bits == 32 ? 0xFFFFFFFFu : (1u << f.bits) - 1;
    return (c[f.word] >> f.shift) & mask;
}

static void set(u32* c, Field f, u32 value) {
    u32 mask = f.bits == 32 ? 0xFFFFFFFFu : (1u << f.bits) - 1;
    c[f.word] = (c[f.word] & ~(mask << f.shift)) | ((value & mask) << f.shift);
}

// Fields an update copies, by update flag bit.
static const Field k_update_fields[] = {
    SIZE_READ0,         SIZE_READ1,          NUM_LOOPS,         VALID_BUFFER,
    {0, 20, 1},         {0, 21, 1},          SIZE_WRITE,        LOOP_SUBFRAME_END,
    LOOP_SUBFRAME_SKIP, SUBFRAMES_TO_DECODE, SUBFRAMES_TO_SKIP, SAMPLE_RATE,
    NUM_CHANNELS,       VALID_WRITE,         OFFSET_READ,       LOOP_START,
    LOOP_END,           PACKET_METADATA,     CURRENT_BUFFER,    PTR_READ0,
    PTR_READ1,          PTR_WRITE,           PTR_OVERLAP_ADD,   WRITE_BUFFER_OFFSET_READ,
    OFFSET_WRITE,
};
static_assert(ARRAY_COUNT(k_update_fields) == 25, "one field per update flag");

// Update flags that move the stream to a new start: the decoder starts over there.
#define UPDATE_RESTART ((1u << 11) | (1u << 12) | (1u << 14)) // sample rate, channels, read offset

// --- state ---------------------------------------------------------------------------------------

#define MAX_CONTEXTS     512
#define PACKET_BYTES     2048u
#define PACKET_BITS      (PACKET_BYTES * 8u)
#define HEADER_BITS      32u
#define SUBFRAMES        4u
#define SUBFRAME_SAMPLES 128u
#define BLOCK_BYTES      256u
#define FINAL_FRAME      0x7FFFu
#define INFINITE_LOOPS   255u

// Our side of a context: decoder state and the decoded frame still going out.
struct Stream {
    XmaDecoder decoder;
    s16 pcm[XMA_FRAME_SAMPLES * 2]; // interleaved
    u8 next;                        // next subframe of pcm to write
    u8 end;                         // subframes of pcm to write
    u8 skip;                        // subframes to drop from the next frame
    u8 restart;                     // the next frame starts a new stream
    u8 warned;
};

static struct {
    SRWLOCK lock;
    u32* contexts; // the title's array, XMA_CONTEXT_SIZE bytes each
    u32 count;
    Stream* streams; // one per context
    u32 enabled[MAX_CONTEXTS / 32];
    bool decoder_ready;
} g_xma = {SRWLOCK_INIT};

static u32* context(u32 index) {
    return g_xma.contexts && g_xma.streams && index < g_xma.count
               ? g_xma.contexts + index * (XMA_CONTEXT_SIZE / 4)
               : nullptr;
}

static bool bit(const u32* set, u32 i) {
    return (set[i / 32] >> (i % 32)) & 1;
}

static void set_bit(u32* set, u32 i, bool on) {
    if (on)
        set[i / 32] |= 1u << (i % 32);
    else
        set[i / 32] &= ~(1u << (i % 32));
}

// Host pointer for `bytes` of APU memory at `address`, or nullptr if any of it is outside.
static u8* apu_range(u32 address, u32 bytes) {
    u8* p = (u8*)apu_ptr(address);
    return p && bytes && apu_ptr(address + bytes - 1) == p + bytes - 1 ? p : nullptr;
}

void xma_set_contexts(void* contexts, u32 count) {
    AcquireSRWLockExclusive(&g_xma.lock);
    if (g_xma.streams) {
        VirtualFree(g_xma.streams, 0, MEM_RELEASE);
        g_xma.streams = nullptr;
    }
    g_xma.contexts = (u32*)contexts;
    g_xma.count = count < MAX_CONTEXTS ? count : MAX_CONTEXTS;
    memset(g_xma.enabled, 0, sizeof(g_xma.enabled));
    if (contexts && g_xma.count) {
        if (!g_xma.decoder_ready) {
            xma_decoder_init();
            g_xma.decoder_ready = true;
        }
        // Zero pages until a context first decodes, so idle contexts cost nothing.
        g_xma.streams = (Stream*)VirtualAlloc(nullptr, sizeof(Stream) * g_xma.count, MEM_RESERVE | MEM_COMMIT,
                                              PAGE_READWRITE);
        if (!g_xma.streams)
            LOG_E("apu", "XMA: no memory for %u decoder states (%lu); XMA contexts stay idle", g_xma.count,
                  GetLastError());
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_enable(u32 index, bool enable) {
    AcquireSRWLockExclusive(&g_xma.lock);
    if (context(index)) {
        if (enable && !bit(g_xma.enabled, index))
            g_xma.streams[index].restart = 1;
        set_bit(g_xma.enabled, index, enable);
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_update(u32 index, u32 mask, const void* source) {
    AcquireSRWLockExclusive(&g_xma.lock);
    u32* c = context(index);
    const u32* src = (const u32*)source;
    if (c && src) {
        for (u32 b = 0; b < ARRAY_COUNT(k_update_fields); b++)
            if (mask & (1u << b))
                set(c, k_update_fields[b], get(src, k_update_fields[b]));
        if (mask & UPDATE_RESTART)
            g_xma.streams[index].restart = 1;
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_update_entry(u32 index, u32 ptr_read0, u32 ptr_read1, u32 size_read0, u32 size_read1,
                      u32 write_buffer_offset_read, u32 valid_buffer) {
    AcquireSRWLockExclusive(&g_xma.lock);
    if (u32* c = context(index)) {
        set(c, PTR_READ0, ptr_read0);
        set(c, PTR_READ1, ptr_read1);
        set(c, SIZE_READ0, size_read0);
        set(c, SIZE_READ1, size_read1);
        set(c, WRITE_BUFFER_OFFSET_READ, write_buffer_offset_read);
        set(c, VALID_BUFFER, get(c, VALID_BUFFER) | valid_buffer);
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_increment_write_read(u32 index, u32 increment) {
    AcquireSRWLockExclusive(&g_xma.lock);
    if (u32* c = context(index)) {
        u32 blocks = get(c, SIZE_WRITE);
        if (blocks)
            set(c, WRITE_BUFFER_OFFSET_READ, (get(c, WRITE_BUFFER_OFFSET_READ) + increment) % blocks);
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_update_read_buffer(u32 index, u32 buffer, u32 size, bool change_size, bool change_address,
                            u32 address) {
    AcquireSRWLockExclusive(&g_xma.lock);
    if (u32* c = context(index)) {
        if (change_size)
            set(c, buffer ? SIZE_READ1 : SIZE_READ0, size);
        if (change_address)
            set(c, buffer ? PTR_READ1 : PTR_READ0, address);
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

// --- bitstream -----------------------------------------------------------------------------------

// One input buffer, positions in bits from its start (packet headers included).
struct InputBuffer {
    const u8* data;
    u32 bits;
};

static InputBuffer input(const u32* c, u32 buffer) {
    u32 address = get(c, buffer ? PTR_READ1 : PTR_READ0) & ~(PACKET_BYTES - 1);
    u32 packets = get(c, buffer ? SIZE_READ1 : SIZE_READ0);
    InputBuffer in;
    in.data = apu_range(address, packets * PACKET_BYTES);
    in.bits = in.data ? packets * PACKET_BITS : 0;
    return in;
}

// Where this stream continues after the packet holding `pos`: past the packets of other streams
// and the next packet's header. Returns in.bits at the end of the buffer.
static u32 next_packet(const InputBuffer& in, u32 pos) {
    u32 packet_start = pos / PACKET_BITS * PACKET_BITS;
    u32 skip = in.data[packet_start / 8 + 3];
    u32 next = packet_start + (skip + 1) * PACKET_BITS;
    return next < in.bits ? next + HEADER_BITS : in.bits;
}

// Moves `pos` on by `count` bits of this stream's data. Returns in.bits when it runs off the end.
static u32 advance(const InputBuffer& in, u32 pos, u32 count) {
    while (count) {
        u32 left = (pos / PACKET_BITS + 1) * PACKET_BITS - pos;
        if (count < left)
            return pos + count;
        count -= left;
        pos = next_packet(in, pos);
        if (pos >= in.bits)
            return in.bits;
    }
    return pos;
}

static u32 read_bits15(const InputBuffer& in, u32 pos) {
    u32 v = 0;
    for (u32 i = 0; i < 15 && pos < in.bits; i++) {
        v = (v << 1) | ((in.data[pos >> 3] >> (7 - (pos & 7))) & 1);
        pos = advance(in, pos, 1);
    }
    return v;
}

// The first frame of the buffer: just past the first packet's header and the frame data
// continuing from the previous packet.
// The first frame starting in the packet whose data begins at `pos` (just past its header): the
// header gives the bits of the frame continuing from the previous packet.
static u32 first_frame(const InputBuffer& in, u32 pos) {
    const u8* h = in.data + (pos - HEADER_BITS) / 8;
    return pos + (((u32)(h[0] & 3) << 13) | ((u32)h[1] << 5) | (h[2] >> 3));
}

// Whether a frame starts at `pos`, or only padding follows to the end of the packet: a length of
// all ones, or too few bits for a length field without the next packet continuing a frame.
static bool frame_starts(const InputBuffer& in, u32 pos) {
    u32 left = (pos / PACKET_BITS + 1) * PACKET_BITS - pos;
    if (left >= 15)
        return read_bits15(in, pos) != FINAL_FRAME;
    u32 next = next_packet(in, pos);
    return next < in.bits && first_frame(in, next) != next;
}

// Copies the frame at `pos` (`length` bits) into `out`, its pieces joined across packets, starting
// at bit pos & 7 of the first byte. Pieces after the first start right after a packet header, so
// they join byte-aligned. False if the frame runs past the buffer.
static bool gather_frame(const InputBuffer& in, u32 pos, u32 length, u8* out) {
    u32 size = 0;
    while (length) {
        u32 piece = (pos / PACKET_BITS + 1) * PACKET_BITS - pos;
        if (piece > length)
            piece = length;
        u32 first = pos >> 3, last = (pos + piece + 7) >> 3;
        memcpy(out + size, in.data + first, last - first);
        size += last - first;
        length -= piece;
        if (length) {
            pos = next_packet(in, pos);
            if (pos >= in.bits)
                return false;
        }
    }
    memset(out + size, 0, XMA_FRAME_PADDING);
    return true;
}

// --- decoding ------------------------------------------------------------------------------------

// The current buffer is used up: hand it back to the title and move to the other one.
static void finish_buffer(u32* c) {
    u32 current = get(c, CURRENT_BUFFER);
    set(c, VALID_BUFFER, get(c, VALID_BUFFER) & ~(1u << current));
    set(c, CURRENT_BUFFER, current ^ 1);
    set(c, OFFSET_READ, 0);
}

static void parser_error(u32* c, u32 index, u32 status, u32 pos) {
    LOG_W("apu", "XMA context %u: parser error %u at bit %u; context disabled", index, status, pos);
    set(c, PARSER_ERROR_STATUS, status);
    set(c, PARSER_ERROR_SET, 1);
    set_bit(g_xma.enabled, index, false);
}

// Decodes the next frame into the stream's pending samples. Returns false when there is no input.
static bool decode_next(u32* c, u32 index, Stream* st, u32 channels) {
    for (int guard = 0; guard < 8; guard++) {
        u32 current = get(c, CURRENT_BUFFER);
        if (!(get(c, VALID_BUFFER) & (1u << current)))
            return false; // waiting for the title to refill
        InputBuffer in = input(c, current);
        if (in.bits < PACKET_BITS) {
            finish_buffer(c);
            continue;
        }
        u32 pos = get(c, OFFSET_READ);
        if (pos < HEADER_BITS)
            pos = first_frame(in, HEADER_BITS);
        // Packets end in padding when the next frame starts a new block; the stream goes on in its
        // next packet, and the buffer is used up after its last one.
        for (u32 skip = 0; pos < in.bits && !frame_starts(in, pos) && skip < in.bits / PACKET_BITS; skip++) {
            u32 next = next_packet(in, pos);
            pos = next < in.bits ? first_frame(in, next) : in.bits;
        }
        if (pos >= in.bits) {
            finish_buffer(c);
            continue;
        }
        u32 length = read_bits15(in, pos);
        if (length <= 15) {
            parser_error(c, index, 4, pos); // bitstream corruption
            return false;
        }
        u32 next = advance(in, pos, length);

        alignas(8) u8 frame[XMA_FRAME_MAX_BYTES + XMA_FRAME_PADDING];
        if (!gather_frame(in, pos, length, frame)) {
            finish_buffer(c); // a frame never continues in the other buffer
            continue;
        }
        if (st->restart) {
            xma_decoder_reset(&st->decoder, channels, get(c, SAMPLE_RATE));
            st->skip = (u8)get(c, SUBFRAMES_TO_SKIP);
            st->restart = 0;
        }
        if (!xma_decoder_frame(&st->decoder, frame, pos & 7, length, st->pcm) && !st->warned) {
            LOG_W("apu", "XMA context %u: corrupt frame at bit %u (%u bits), played as silence", index, pos,
                  length);
            st->warned = 1;
        }
        st->next = st->skip < SUBFRAMES ? st->skip : SUBFRAMES;
        st->end = SUBFRAMES;
        st->skip = 0;

        // The loop end frame plays up to its loop end subframe, then the loop start frame follows,
        // minus its subframes before the loop start.
        u32 loops = get(c, NUM_LOOPS), loop_end = get(c, LOOP_END);
        if (loops && pos <= loop_end && loop_end < next) {
            st->end = (u8)(get(c, LOOP_SUBFRAME_END) + 1);
            st->skip = (u8)get(c, LOOP_SUBFRAME_SKIP);
            if (loops != INFINITE_LOOPS)
                set(c, NUM_LOOPS, loops - 1);
            set(c, OFFSET_READ, get(c, LOOP_START));
        } else if (next >= in.bits) {
            finish_buffer(c);
        } else {
            set(c, OFFSET_READ, next);
        }
        return true;
    }
    return false;
}

// Writes decoded subframes into one context's output ring until input or room runs out. One block
// always stays free, so a full ring can't look empty.
static void run_context(u32 index) {
    u32* c = context(index);
    Stream* st = &g_xma.streams[index];
    if (!get(c, VALID_WRITE))
        return;
    u32 blocks = get(c, SIZE_WRITE);
    u8* ring = apu_range(get(c, PTR_WRITE), blocks * BLOCK_BYTES);
    if (!ring)
        return;
    u32 channels = get(c, NUM_CHANNELS) + 1;
    u32 subframe_blocks = channels; // 128 16-bit samples per channel
    if (st->restart)
        st->next = st->end = 0; // drop what was decoded for the old stream position

    for (int guard = 0; guard < 64; guard++) {
        // Offsets are 5-bit fields the title can leave past a ring it shrank: wrap them.
        u32 write = get(c, OFFSET_WRITE) % blocks, read = get(c, WRITE_BUFFER_OFFSET_READ) % blocks;
        u32 free = blocks - 1 - (write + blocks - read) % blocks;
        if (free < subframe_blocks)
            return; // ring full: nothing is decoded ahead of room for it
        if (st->next >= st->end) {
            if (!decode_next(c, index, st, channels))
                return;
            continue;
        }
        while (st->next < st->end && free >= subframe_blocks) {
            const u8* src = (const u8*)(st->pcm + st->next * SUBFRAME_SAMPLES * channels);
            for (u32 b = 0; b < subframe_blocks; b++, write = (write + 1) % blocks)
                memcpy(ring + (usize)write * BLOCK_BYTES, src + (usize)b * BLOCK_BYTES, BLOCK_BYTES);
            free -= subframe_blocks;
            st->next++;
        }
        set(c, OFFSET_WRITE, write);
    }
}

void xma_run() {
    for (u32 w = 0; w < MAX_CONTEXTS / 32; w++) {
        AcquireSRWLockShared(&g_xma.lock);
        u32 bits = g_xma.enabled[w];
        ReleaseSRWLockShared(&g_xma.lock);
        for (; bits; bits &= bits - 1) {
            unsigned long b;
            _BitScanForward(&b, bits);
            u32 index = w * 32 + b;
            // One context at a time under the lock, so commands never wait long.
            AcquireSRWLockExclusive(&g_xma.lock);
            if (bit(g_xma.enabled, index) && context(index))
                run_context(index);
            ReleaseSRWLockExclusive(&g_xma.lock);
        }
    }
}
