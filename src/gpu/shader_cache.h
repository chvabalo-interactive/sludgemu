// Translated shader programs kept on disk between runs: content seen in an earlier run is ready at
// once instead of being translated and compiled again.
//
// One file per program, named by its key, in a folder per cache generation. The generation is a value
// that changes whenever translating the same program could come out different (the translator's
// sources, the shader compiler, what the device runs); folders of generations no longer used are
// pruned after a while. Every file carries its generation, key and a checksum, and is written under a
// temporary name and renamed into place, so a file that doesn't match in every respect is a miss.
//
// The generation folder also keeps a log of fixed-size records for the pipeline layer: the desktop
// pipelines titles drew, so that they can be made before the first draw next time.

#pragma once

#include "common/types.h"
#include "shader/translate.h"

// A built program: its DXIL and what pipelines need to know about it.
struct ShaderBinary {
    void* dxil; // malloc'ed
    usize dxil_size;
    u32 vertex_element_mask;
    u32 element_format[SHADER_MAX_VERTEX_ELEMENTS];
    u32 ps_target_mask;
    bool ps_writes_depth;
    u32 sampler_count; // inline samplers: static samplers in the pipeline's root signature
    InlineSampler samplers[SHADER_MAX_INLINE_SAMPLERS];
    u16 table_reach[SHADER_MAX_ROOT_PARAMETERS]; // see TranslatedShader
    u32 vs_param_mask;                           // vertex shaders: the parameters they export
};

// Uses (and creates) `root`\<generation>\ for programs and records. False if the folder can't be
// made; the cache then stays closed and every call misses.
bool shader_cache_open(const wchar_t* root, u64 generation);
void shader_cache_close();

// The folder in use (with a trailing backslash), or an empty string while closed.
const wchar_t* shader_cache_folder();

// The program with `key` (and `code_size` bytes of console code), if this generation has it. Fills
// `out`, its DXIL newly allocated.
bool shader_cache_load(u64 key, u32 code_size, ShaderBinary* out);

// Writes the program with `key`. False (unlogged) if it can't; the cache is only an optimization.
bool shader_cache_store(u64 key, u32 code_size, const ShaderBinary& binary);

// Deletes generation folders under the root other than the open one: those unused for
// `max_age_days`, and all but the `keep` most recently used. Returns the number of folders removed.
u32 shader_cache_prune(u32 max_age_days, u32 keep);

// The record log. A record is an ID and data opaque to the cache.
#define CACHE_RECORD_DATA_BYTES 16

struct CacheRecord {
    u64 id;
    u8 data[CACHE_RECORD_DATA_BYTES];
};

// Reads up to `capacity` records, in the order they were appended. A damaged or truncated tail ends
// the log there (later appends replace it). Returns the number read.
u32 shader_cache_read_records(CacheRecord* out, u32 capacity);

// Appends a record. Thread safe; the write may block, so callers keep it off threads titles wait for.
bool shader_cache_append_record(const CacheRecord& record);
