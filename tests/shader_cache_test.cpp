// The shader cache: programs and records round trip, and anything that doesn't match exactly (another
// generation, another program, damage, truncation) misses instead of loading.

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "gpu/shader_cache.h"
#include "test.h"

static void make_binary(ShaderBinary* b, u32 seed) {
    memset(b, 0, sizeof(*b));
    b->dxil_size = 1000 + seed;
    b->dxil = malloc(b->dxil_size);
    for (usize i = 0; i < b->dxil_size; i++)
        ((u8*)b->dxil)[i] = (u8)(i * 7 + seed);
    b->vertex_element_mask = 0x5 + seed;
    b->element_format[3] = 41;
    b->ps_target_mask = 0x3;
    b->ps_writes_depth = true;
    b->sampler_count = 2;
    b->samplers[1].filter = 0x55;
    b->samplers[1].max_lod = 13.5f;
    b->table_reach[7] = 300;
    b->table_reach[8] = 0xFFFF;
    b->vs_param_mask = 0x1F;
}

static bool same(const ShaderBinary& a, const ShaderBinary& b) {
    return a.dxil_size == b.dxil_size && memcmp(a.dxil, b.dxil, a.dxil_size) == 0 &&
           a.vertex_element_mask == b.vertex_element_mask &&
           memcmp(a.element_format, b.element_format, sizeof(a.element_format)) == 0 &&
           a.ps_target_mask == b.ps_target_mask && a.ps_writes_depth == b.ps_writes_depth &&
           a.sampler_count == b.sampler_count && memcmp(a.samplers, b.samplers, sizeof(a.samplers)) == 0 &&
           memcmp(a.table_reach, b.table_reach, sizeof(a.table_reach)) == 0 &&
           a.vs_param_mask == b.vs_param_mask;
}

static bool load_matches(u64 key, u32 code_size, const ShaderBinary& expected) {
    ShaderBinary got;
    if (!shader_cache_load(key, code_size, &got))
        return false;
    bool ok = same(got, expected);
    free(got.dxil);
    return ok;
}

// Damages a stored program file: `cut` bytes off its end, or else the byte at `flip` flipped (counted
// from the end when negative).
static bool damage(u64 key, u32 cut, s64 flip) {
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s%016llx.prog", shader_cache_folder(), (unsigned long long)key);
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) != 0;
    if (cut) {
        LARGE_INTEGER end;
        end.QuadPart = size.QuadPart - cut;
        ok = ok && SetFilePointerEx(f, end, nullptr, FILE_BEGIN) && SetEndOfFile(f);
    } else {
        LARGE_INTEGER at;
        at.QuadPart = flip < 0 ? size.QuadPart + flip : flip;
        u8 byte = 0;
        DWORD n = 0;
        ok = ok && SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, &byte, 1, &n, nullptr) &&
             n == 1;
        byte ^= 0x10;
        ok = ok && SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && WriteFile(f, &byte, 1, &n, nullptr) &&
             n == 1;
    }
    CloseHandle(f);
    return ok;
}

static void append_garbage_to_records(u32 bytes) {
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%spipelines.log", shader_cache_folder());
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    u8 junk[64];
    memset(junk, 0xAB, sizeof(junk));
    DWORD n = 0;
    WriteFile(f, junk, bytes, &n, nullptr);
    CloseHandle(f);
}

static void remove_tree(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            _snwprintf(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(path);
            else
                DeleteFileW(path);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir);
}

// Dates a folder's last write `seconds` back (pruning goes by it).
static void set_written_ago(const wchar_t* folder, u64 seconds) {
    HANDLE h =
        CreateFileW(folder, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE)
        return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    u64 t = (((u64)now.dwHighDateTime << 32) | now.dwLowDateTime) - seconds * 10000000ull;
    FILETIME then = {(DWORD)t, (DWORD)(t >> 32)};
    CHECK(SetFileTime(h, nullptr, nullptr, &then));
    CloseHandle(h);
}

void test_shader_cache() {
    wchar_t temp[MAX_PATH], root[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    _snwprintf(root, MAX_PATH, L"%ssludgemu_cache_test_%lu", temp, GetCurrentProcessId());
    remove_tree(root);

    ShaderBinary a, b;
    make_binary(&a, 1);
    make_binary(&b, 2);
    const u64 gen1 = 0x1111222233334444ull, gen2 = 0x5555666677778888ull;

    // Round trip, by key and code size.
    CHECK(shader_cache_open(root, gen1));
    CHECK(!load_matches(0xA, 64, a)); // nothing yet
    CHECK(shader_cache_store(0xA, 64, a));
    CHECK(shader_cache_store(0xB, 96, b));
    CHECK(load_matches(0xA, 64, a));
    CHECK(load_matches(0xB, 96, b));
    CHECK(!load_matches(0xA, 65, a));      // same key, other code: a collision, not a hit
    CHECK(shader_cache_store(0xA, 64, b)); // replaced whole
    CHECK(load_matches(0xA, 64, b));
    CHECK(shader_cache_store(0xA, 64, a));

    // Damage: a flipped DXIL byte, a flipped header byte, truncation.
    CHECK(damage(0xB, 0, -1));
    CHECK(!load_matches(0xB, 96, b));
    CHECK(shader_cache_store(0xB, 96, b));
    CHECK(damage(0xB, 0, 17)); // in the key
    ShaderBinary got;
    CHECK(!shader_cache_load(0xB, 96, &got));
    CHECK(shader_cache_store(0xB, 96, b));
    CHECK(damage(0xB, 5, 0));
    CHECK(!shader_cache_load(0xB, 96, &got));
    CHECK(load_matches(0xA, 64, a)); // others unaffected

    // Records: round trip, and a damaged tail ends the log there.
    CacheRecord records[8];
    CHECK(shader_cache_read_records(records, 8) == 0);
    CacheRecord r1 = {0x1234, {1, 2, 3}}, r2 = {0x5678, {4, 5, 6, 7}};
    CHECK(shader_cache_append_record(r1));
    CHECK(shader_cache_append_record(r2));
    shader_cache_close();

    // Another generation sees none of it: not the programs, not the records.
    CHECK(shader_cache_open(root, gen2));
    CHECK(!load_matches(0xA, 64, a));
    CHECK(shader_cache_read_records(records, 8) == 0);
    shader_cache_close();

    CHECK(shader_cache_open(root, gen1));
    CHECK(load_matches(0xA, 64, a));
    CHECK(shader_cache_read_records(records, 8) == 2);
    CHECK(records[0].id == r1.id && memcmp(records[0].data, r1.data, sizeof(r1.data)) == 0);
    CHECK(records[1].id == r2.id && memcmp(records[1].data, r2.data, sizeof(r2.data)) == 0);
    CHECK(shader_cache_read_records(records, 1) == 1); // capacity: the rest is dropped
    shader_cache_close();
    CHECK(shader_cache_open(root, gen1));
    CHECK(shader_cache_read_records(records, 8) == 1);
    shader_cache_close();
    append_garbage_to_records(20); // an interrupted append
    CHECK(shader_cache_open(root, gen1));
    CHECK(shader_cache_read_records(records, 8) == 1);
    CHECK(shader_cache_append_record(r2)); // continues after the last good record
    shader_cache_close();
    append_garbage_to_records(32); // a whole record's worth of damage
    CHECK(shader_cache_open(root, gen1));
    CHECK(shader_cache_read_records(records, 8) == 2);
    CHECK(records[1].id == r2.id);

    // Pruning keeps the open generation and recent ones, removes old ones.
    wchar_t old_folder[MAX_PATH];
    _snwprintf(old_folder, MAX_PATH, L"%s\\%016llx", root, (unsigned long long)gen2);
    CHECK(shader_cache_prune(14, 4) == 0);
    set_written_ago(old_folder, 30ull * 24 * 3600);
    CHECK(shader_cache_prune(14, 4) == 1);
    CHECK(GetFileAttributesW(old_folder) == INVALID_FILE_ATTRIBUTES);
    CHECK(load_matches(0xA, 64, a));

    // Past the most recently used few, generations go whatever their age: of six recent ones (made
    // a minute apart, the first one longest ago), the two used longest ago go.
    wchar_t recent[6][MAX_PATH];
    for (u32 i = 0; i < 6; i++) {
        _snwprintf(recent[i], MAX_PATH, L"%s\\%016llx", root, 0xAB00000000000000ull + i);
        CHECK(CreateDirectoryW(recent[i], nullptr));
        set_written_ago(recent[i], (6 - i) * 60ull);
    }
    CHECK(shader_cache_prune(14, 4) == 2);
    for (u32 i = 0; i < 6; i++)
        CHECK((GetFileAttributesW(recent[i]) == INVALID_FILE_ATTRIBUTES) == (i < 2));
    CHECK(shader_cache_prune(14, 0) == 4); // keeping none: all but the open one
    CHECK(load_matches(0xA, 64, a));
    shader_cache_close();

    // Closed: everything misses.
    CHECK(!shader_cache_load(0xA, 64, &got));
    CHECK(!shader_cache_store(0xA, 64, a));
    free(a.dxil);
    free(b.dxil);
    remove_tree(root);
}
