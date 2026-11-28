#include "gpu/shader_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "common/log.h"

// File layouts. Bump a format when its layout changes (the generation covers what goes into them).
#define PROGRAM_MAGIC  0x50434c53u // "SLCP"
#define PROGRAM_FORMAT 1u
#define RECORDS_MAGIC  0x52434c53u // "SLCR"
#define RECORDS_FORMAT 1u
#define RECORDS_FILE   L"pipelines.log"

// Nothing translated comes near this; a larger file is damaged.
#define PROGRAM_MAX_FILE (64u << 20)

struct ProgramHeader {
    u32 magic;
    u32 format;
    u64 generation;
    u64 key;
    u32 code_size;
    u32 payload_size;
    u64 checksum; // of the payload
};

struct RecordsHeader {
    u32 magic;
    u32 format;
    u64 generation;
};

struct StoredRecord {
    CacheRecord record;
    u64 check; // of the record, seeded with the generation
};

static_assert(sizeof(ProgramHeader) == 40, "program file header layout");
static_assert(sizeof(StoredRecord) == 32, "record layout");

static struct {
    bool open;
    u64 generation;
    wchar_t root[MAX_PATH];   // with a trailing backslash
    wchar_t folder[MAX_PATH]; // root + generation, with a trailing backslash
    HANDLE records;           // INVALID_HANDLE_VALUE when there is no log
    SRWLOCK records_lock;
} g_cache = {false, 0, {}, {}, INVALID_HANDLE_VALUE, SRWLOCK_INIT};

static u64 hash_bytes(const void* data, usize size, u64 h) {
    const u8* p = (const u8*)data;
    for (usize i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

#define HASH_BASIS 0xcbf29ce484222325ull

// --- opening -----------------------------------------------------------------------------------------

static bool make_directory(const wchar_t* path) {
    return CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Marks the folder as used now: pruning goes by its last write time, which files written into it
// update too, but a run that finds everything cached writes nothing.
static void touch(const wchar_t* folder) {
    HANDLE h =
        CreateFileW(folder, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(h, nullptr, nullptr, &now);
    CloseHandle(h);
}

bool shader_cache_open(const wchar_t* root, u64 generation) {
    shader_cache_close();
    usize n = wcslen(root);
    // Room for the generation folder and a file name in it (key and temporary suffix).
    if (!n || n + 64 >= MAX_PATH)
        return false;
    const wchar_t* separator = root[n - 1] == L'\\' || root[n - 1] == L'/' ? L"" : L"\\";
    _snwprintf(g_cache.root, MAX_PATH, L"%s%s", root, separator);
    _snwprintf(g_cache.folder, MAX_PATH, L"%s%016llx\\", g_cache.root, (unsigned long long)generation);
    if (!make_directory(g_cache.root) || !make_directory(g_cache.folder)) {
        g_cache.root[0] = g_cache.folder[0] = 0;
        return false;
    }
    touch(g_cache.folder);
    g_cache.generation = generation;
    // One process at a time keeps the log (a second sludgemu running the title goes without).
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s%s", g_cache.folder, RECORDS_FILE);
    g_cache.records = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    g_cache.open = true;
    return true;
}

void shader_cache_close() {
    if (g_cache.records != INVALID_HANDLE_VALUE)
        CloseHandle(g_cache.records);
    g_cache.records = INVALID_HANDLE_VALUE;
    g_cache.open = false;
    g_cache.root[0] = g_cache.folder[0] = 0;
}

const wchar_t* shader_cache_folder() {
    return g_cache.folder;
}

// --- programs ----------------------------------------------------------------------------------------

static void program_path(wchar_t* out, u64 key, const wchar_t* suffix) {
    _snwprintf(out, MAX_PATH, L"%s%016llx%s", g_cache.folder, (unsigned long long)key, suffix);
}

// The payload: fixed fields, then the DXIL.
#define PAYLOAD_FIXED_BYTES                                                                                  \
    (6 * 4 + SHADER_MAX_VERTEX_ELEMENTS * 4 + SHADER_MAX_ROOT_PARAMETERS * 2 +                               \
     SHADER_MAX_INLINE_SAMPLERS * sizeof(InlineSampler))

struct Cursor {
    u8* at;
    u8* end;
};

static void put(Cursor* c, const void* data, usize size) {
    memcpy(c->at, data, size);
    c->at += size;
}

static void put_u32(Cursor* c, u32 v) {
    put(c, &v, 4);
}

static bool take(Cursor* c, void* data, usize size) {
    if ((usize)(c->end - c->at) < size)
        return false;
    memcpy(data, c->at, size);
    c->at += size;
    return true;
}

bool shader_cache_store(u64 key, u32 code_size, const ShaderBinary& b) {
    if (!g_cache.open || !b.dxil || !b.dxil_size || b.dxil_size > PROGRAM_MAX_FILE / 2)
        return false;
    usize payload = PAYLOAD_FIXED_BYTES + b.dxil_size;
    u8* file = (u8*)malloc(sizeof(ProgramHeader) + payload);
    if (!file)
        return false;
    Cursor c = {file + sizeof(ProgramHeader), file + sizeof(ProgramHeader) + payload};
    put_u32(&c, (u32)b.dxil_size);
    put_u32(&c, b.vertex_element_mask);
    put_u32(&c, b.ps_target_mask);
    put_u32(&c, b.ps_writes_depth ? 1u : 0u);
    put_u32(&c, b.sampler_count);
    put_u32(&c, b.vs_param_mask);
    put(&c, b.element_format, sizeof(b.element_format));
    put(&c, b.table_reach, sizeof(b.table_reach));
    put(&c, b.samplers, sizeof(b.samplers));
    put(&c, b.dxil, b.dxil_size);
    ProgramHeader h = {PROGRAM_MAGIC,
                       PROGRAM_FORMAT,
                       g_cache.generation,
                       key,
                       code_size,
                       (u32)payload,
                       hash_bytes(file + sizeof(h), payload, HASH_BASIS)};
    memcpy(file, &h, sizeof(h));

    // Written whole under a name of this thread's, then renamed: readers (and other processes) see
    // the old file or the new one, never a part.
    wchar_t temp[MAX_PATH], path[MAX_PATH];
    wchar_t suffix[32];
    _snwprintf(suffix, ARRAY_COUNT(suffix), L".%lu.tmp", GetCurrentThreadId());
    program_path(temp, key, suffix);
    program_path(path, key, L".prog");
    bool ok = false;
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        DWORD size = (DWORD)(sizeof(ProgramHeader) + payload);
        ok = WriteFile(f, file, size, &written, nullptr) && written == size;
        CloseHandle(f);
        ok = ok && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING);
        if (!ok)
            DeleteFileW(temp);
    }
    free(file);
    return ok;
}

bool shader_cache_load(u64 key, u32 code_size, ShaderBinary* out) {
    if (!g_cache.open)
        return false;
    wchar_t path[MAX_PATH];
    program_path(path, key, L".prog");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    u8* file = nullptr;
    DWORD read = 0;
    bool whole = GetFileSizeEx(f, &size) && size.QuadPart > (LONGLONG)sizeof(ProgramHeader) &&
                 size.QuadPart <= PROGRAM_MAX_FILE && (file = (u8*)malloc((usize)size.QuadPart)) != nullptr &&
                 ReadFile(f, file, (DWORD)size.QuadPart, &read, nullptr) && read == (DWORD)size.QuadPart;
    CloseHandle(f);
    if (!whole) {
        free(file);
        return false;
    }
    ProgramHeader h;
    memcpy(&h, file, sizeof(h));
    usize payload = (usize)size.QuadPart - sizeof(h);
    bool valid = h.magic == PROGRAM_MAGIC && h.format == PROGRAM_FORMAT &&
                 h.generation == g_cache.generation && h.key == key && h.code_size == code_size &&
                 h.payload_size == payload && payload > PAYLOAD_FIXED_BYTES &&
                 h.checksum == hash_bytes(file + sizeof(h), payload, HASH_BASIS);
    ShaderBinary b;
    memset(&b, 0, sizeof(b));
    Cursor c = {file + sizeof(h), file + sizeof(h) + payload};
    u32 dxil_size = 0, writes_depth = 0;
    valid = valid && take(&c, &dxil_size, 4) && dxil_size == payload - PAYLOAD_FIXED_BYTES &&
            take(&c, &b.vertex_element_mask, 4) && take(&c, &b.ps_target_mask, 4) &&
            take(&c, &writes_depth, 4) && take(&c, &b.sampler_count, 4) &&
            b.sampler_count <= SHADER_MAX_INLINE_SAMPLERS && take(&c, &b.vs_param_mask, 4) &&
            take(&c, b.element_format, sizeof(b.element_format)) &&
            take(&c, b.table_reach, sizeof(b.table_reach)) && take(&c, b.samplers, sizeof(b.samplers));
    if (valid) {
        b.dxil = malloc(dxil_size);
        valid = b.dxil != nullptr && take(&c, b.dxil, dxil_size);
    }
    free(file);
    if (!valid) {
        free(b.dxil);
        return false;
    }
    b.dxil_size = dxil_size;
    b.ps_writes_depth = writes_depth != 0;
    *out = b;
    return true;
}

// --- pruning -----------------------------------------------------------------------------------------

static bool is_generation_name(const wchar_t* name) {
    for (u32 i = 0; i < 16; i++)
        if (!iswxdigit(name[i]))
            return false;
    return name[16] == 0;
}

static void remove_folder(const wchar_t* folder) {
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*", folder);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                _snwprintf(path, MAX_PATH, L"%s\\%s", folder, fd.cFileName);
                DeleteFileW(path);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(folder);
}

#define PRUNE_MAX_KEEP 8

// Calls `visit(name, last write time, context)` for each generation folder under the root other than
// the open one. False if the root can't be listed.
static bool for_other_generations(void (*visit)(const wchar_t* name, u64 written, void* context),
                                  void* context) {
    wchar_t pattern[MAX_PATH], current[17];
    _snwprintf(pattern, MAX_PATH, L"%s*", g_cache.root);
    _snwprintf(current, ARRAY_COUNT(current), L"%016llx", (unsigned long long)g_cache.generation);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !is_generation_name(fd.cFileName) ||
            _wcsicmp(fd.cFileName, current) == 0)
            continue;
        u64 written = ((u64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        visit(fd.cFileName, written, context);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return true;
}

struct Pruning {
    u32 keep;
    u32 newest_count;
    u64 newest[PRUNE_MAX_KEEP]; // last write times of the most recently used, newest first
    u64 oldest_allowed;         // folders written before this go
    u32 removed;
};

static void note_generation(const wchar_t*, u64 written, void* context) {
    Pruning* p = (Pruning*)context;
    u32 at = p->newest_count < p->keep ? p->newest_count++ : p->keep;
    for (; at > 0 && p->newest[at - 1] < written; at--)
        if (at < p->keep)
            p->newest[at] = p->newest[at - 1];
    if (at < p->keep)
        p->newest[at] = written;
}

static void prune_generation(const wchar_t* name, u64 written, void* context) {
    Pruning* p = (Pruning*)context;
    if (written >= p->oldest_allowed)
        return;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s%s", g_cache.root, name);
    remove_folder(path);
    p->removed++;
}

u32 shader_cache_prune(u32 max_age_days, u32 keep) {
    if (!g_cache.open)
        return 0;
    Pruning p = {};
    p.keep = keep < PRUNE_MAX_KEEP ? keep : PRUNE_MAX_KEEP;
    if (p.keep && !for_other_generations(note_generation, &p))
        return 0;
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    u64 now = ((u64)now_ft.dwHighDateTime << 32) | now_ft.dwLowDateTime;
    u64 max_age = (u64)max_age_days * 24 * 3600 * 10000000ull; // FILETIME counts 100 ns
    p.oldest_allowed = max_age < now ? now - max_age + 1 : 0;
    // Every build of the translator is a generation of its own, so developers make many: past the
    // few most recently used, the rest go whatever their age.
    if (p.newest_count == p.keep) {
        u64 kept = p.keep ? p.newest[p.keep - 1] : ~0ull;
        if (kept > p.oldest_allowed)
            p.oldest_allowed = kept;
    }
    for_other_generations(prune_generation, &p);
    return p.removed;
}

// --- records -----------------------------------------------------------------------------------------

static u64 record_check(const CacheRecord& r) {
    return hash_bytes(&r, sizeof(r), HASH_BASIS ^ g_cache.generation);
}

static bool write_at(HANDLE f, u64 offset, const void* data, u32 size) {
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)offset;
    DWORD written = 0;
    return SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && WriteFile(f, data, size, &written, nullptr) &&
           written == size;
}

u32 shader_cache_read_records(CacheRecord* out, u32 capacity) {
    HANDLE f = g_cache.records;
    if (!g_cache.open || f == INVALID_HANDLE_VALUE)
        return 0;
    AcquireSRWLockExclusive(&g_cache.records_lock);
    LARGE_INTEGER start = {};
    SetFilePointerEx(f, start, nullptr, FILE_BEGIN);
    RecordsHeader h = {};
    DWORD read = 0;
    u32 count = 0;
    u64 valid_end = 0;
    if (ReadFile(f, &h, sizeof(h), &read, nullptr) && read == sizeof(h) && h.magic == RECORDS_MAGIC &&
        h.format == RECORDS_FORMAT && h.generation == g_cache.generation) {
        valid_end = sizeof(h);
        StoredRecord batch[256];
        bool more = true;
        while (more && count < capacity) {
            if (!ReadFile(f, batch, sizeof(batch), &read, nullptr) || read == 0)
                break;
            u32 n = read / sizeof(StoredRecord);
            more = read == sizeof(batch);
            for (u32 i = 0; i < n && count < capacity; i++) {
                if (batch[i].check != record_check(batch[i].record)) {
                    more = false;
                    break;
                }
                out[count++] = batch[i].record;
                valid_end += sizeof(StoredRecord);
            }
            if (n * sizeof(StoredRecord) != read)
                more = false; // a partial record: the tail of an interrupted append
        }
    }
    // A log of another generation or format, or a damaged tail, is cut back to what was read; past
    // `capacity`, the rest is dropped too (the log only ever grows by what is new to the reader).
    bool ok = true;
    if (valid_end == 0) {
        RecordsHeader fresh = {RECORDS_MAGIC, RECORDS_FORMAT, g_cache.generation};
        ok = write_at(f, 0, &fresh, sizeof(fresh));
        valid_end = sizeof(fresh);
    }
    LARGE_INTEGER end;
    end.QuadPart = (LONGLONG)valid_end;
    ok = ok && SetFilePointerEx(f, end, nullptr, FILE_BEGIN) && SetEndOfFile(f);
    if (!ok) {
        CloseHandle(f);
        g_cache.records = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_cache.records_lock);
    return count;
}

bool shader_cache_append_record(const CacheRecord& record) {
    if (!g_cache.open)
        return false;
    StoredRecord s;
    memset(&s, 0, sizeof(s));
    s.record = record;
    s.check = record_check(s.record);
    AcquireSRWLockExclusive(&g_cache.records_lock);
    bool ok = false;
    HANDLE f = g_cache.records;
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER zero = {};
        DWORD written = 0;
        ok = SetFilePointerEx(f, zero, nullptr, FILE_END) && WriteFile(f, &s, sizeof(s), &written, nullptr) &&
             written == sizeof(s);
    }
    ReleaseSRWLockExclusive(&g_cache.records_lock);
    return ok;
}
