// fnxf_xiii (2026-10-25, fnxf_xiii_diag_texture_sources): where each texture Renut uploads came from in the game's files,
// for replacing textures by their source (button prompts, controller pictures) without hashing texture data.
//
// Three records, kept by guest address (the newest writer of an address wins; older records it overlaps go):
//   - reads: every NtReadFile (the SDK's g_read_observer, sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp): the buffer holds
//     the file's bytes from the read's offset;
//   - inflates: every native inflate call (fnxf_xiii_native_zlib.cpp): its output holds a decompressed stream's bytes
//     from the stream's output offset. A stream's compressed data is found, at its first call, in a read's buffer:
//     the stream is then the file's bytes from (read offset + position in the buffer - bytes the stream used before);
//   - textures: at Renut's upload of a texture (render/textures.cpp Upload), its base address is looked up in both.
// Logged once per texture (header address, base address, size, format), in the game log, as
//   [texsrc] tex <header> <w>x<h> fmt <f> base <addr> <- <file> @<offset> [zlib stream @<compressed start>
//            +<decompressed offset>]
// The archive's file list (fileCode|pos:usize:csize:path) gives the entry from the offset (the entry's start) and the
// texture's place in the entry (the decompressed offset). A base address in no record: "<- ?" (copied there by the game
// from elsewhere, or made by it).
#include "cpu/fnxf_xiii_texture_sources.h"

#include "cpu/fnxf_xiii_wpd_patch.h"  // fnxf_xiii (2026-10-25): a language pack's changes

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>
#include <cstring>
#include <utility>

#include <fmt/format.h>
#include <xxhash.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_texture_sources, false, "fnxf_xiii",
                    "Logs, once per texture, the game file (and offset, and zlib stream) its data was loaded from: "
                    "for texture replacement by source file")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnxf_xiii_diag_load_watch, false, "fnxf_xiii",
                    "Diagnostic (2026-11-16, the damaged loading screens: the first 64 KB of the picture is 0xCC and "
                    "something else in guest memory): the game's file reads (NtReadFile, NtReadFileScatter) and "
                    "zlib outputs are remembered (the last 16384: where, how much, which file and offset, which thread, "
                    "when); at a texture's upload whose data has a 4 KB page of 0xCC bytes ([loadwatch] DAMAGED), "
                    "and for the first 40 large untiled ones ([loadwatch] ok), the ones that wrote into it are logged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace fnxf_xiii::native_crt {  // cpu/fnxf_xiii_native_crt.cpp (fnxf_xiii_diag_load_watch)
extern std::atomic<void (*)(char kind, uint32_t d, uint32_t s, uint32_t n, uint32_t lr)> g_big_write_observer;
}
namespace rex::kernel::xboxkrnl {
extern std::atomic<void (*)(const std::string &path, uint64_t offset, uint32_t guest_buffer, uint32_t bytes)>
    g_read_observer;
extern std::atomic<void (*)(const std::string &path, uint64_t offset, uint32_t guest_buffer, uint32_t bytes)>
    g_scatter_observer;  // sdk/src/system/xfile.cpp (fnxf_xiii_diag_load_watch)
}

namespace {

// Addresses by what they hold: a guest virtual address in a physical heap (0xA0000000 and up) as its physical address
// with bit 32 set (the textures' base addresses are physical; the same memory has several virtual views), any other as
// itself.
using Key = uint64_t;
constexpr Key kPhysical = 1ull << 32;

Key FromVirtual(uint32_t va) {
  const uint32_t pa = REX_KERNEL_MEMORY()->GetPhysicalAddress(va);
  return pa != UINT32_MAX ? kPhysical | pa : Key(va);
}

struct Range {
  Key end = 0;                // one past the last byte
  const std::string *path = nullptr;  // interned file path
  uint64_t file_offset = 0;   // the file's offset of the range's first byte (reads), or of the stream's start (zlib)
  bool zlib = false;
  uint64_t out_offset = 0;    // zlib: the stream's decompressed offset of the range's first byte
};

std::mutex g_mutex;
std::set<std::string> g_paths;         // interned paths (never erased: a few hundred files)
std::map<Key, Range> g_ranges;         // by first address (Key), not overlapping
struct Stream {
  const std::string *path = nullptr;
  uint64_t start = 0;  // the file offset of the stream's first compressed byte
};
std::unordered_map<uint32_t, Stream> g_streams;  // guest z_stream -> its source (found at its first inflate)
std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> g_logged;  // header, base, size, format
uint64_t g_textures_logged = 0, g_textures_unknown = 0;

const std::string *Intern(const std::string &path) { return &*g_paths.insert(path).first; }

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_load_watch): the last writes of file data into guest memory (g_mutex held).
struct Write {
  uint64_t ns = 0;
  Key start = 0;
  uint32_t bytes = 0;
  uint32_t guest_va = 0;
  const std::string *path = nullptr;  // null: a zlib stream whose source is unknown
  uint64_t offset = 0;                // reads: the file offset; zlib: the stream's output offset
  uint32_t strm = 0;                  // zlib: the guest z_stream
  uint32_t thread = 0;
  uint32_t src = 0;                   // c / m / M: the source; f: the fill value
  uint32_t lr = 0;                    // c / m / M / f: the caller's return address
  char kind = 'r';                    // r NtReadFile, s NtReadFileScatter, z inflate, c memcpy, m memmove,
                                      // M the game's memmove sub_82729978, f memset
};
uint64_t g_reads_seen = 0, g_crt_seen = 0;
// fnxf_xiii (2026-11-16): the hash of each whole 4 KB page a read filled, as it was read (by guest address).
struct PageRead {
  uint64_t hash = 0, ns = 0;
  uint64_t file_offset = 0;
  const std::string *path = nullptr;
};
std::unordered_map<uint32_t, PageRead> g_read_pages;
constexpr size_t kWrites = 16384;
std::vector<Write> g_writes;
uint64_t g_write_count = 0;

uint64_t NowNsLw() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint32_t ThreadTag() {
  thread_local const uint32_t tag = uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  return tag;
}
void RecordWrite(char kind, uint32_t va, uint32_t bytes, const std::string *path, uint64_t offset, uint32_t strm) {
  if (g_writes.empty())
    g_writes.resize(kWrites);
  Write &w = g_writes[g_write_count++ % kWrites];
  w.ns = NowNsLw();
  w.start = FromVirtual(va);
  w.bytes = bytes;
  w.guest_va = va;
  w.path = path;
  w.offset = offset;
  w.strm = strm;
  w.thread = ThreadTag();
  w.src = 0;
  w.lr = 0;
  w.kind = kind;
  ++(kind == 'r' || kind == 's' ? g_reads_seen : g_crt_seen);
}
bool Tracking();

void Insert(Key start, const Range &range) {
  if (range.end <= start)
    return;
  // Cut what it overlaps: a record starting before it keeps its part below; one ending after it keeps its part above.
  auto it = g_ranges.lower_bound(start);
  if (it != g_ranges.begin()) {
    auto prev = std::prev(it);
    if (prev->second.end > start) {
      Range above = prev->second;
      const Key old_end = above.end;
      prev->second.end = start;
      if (old_end > range.end) {
        const uint64_t skip = range.end - prev->first;
        above.file_offset += above.zlib ? 0 : skip;
        above.out_offset += above.zlib ? skip : 0;
        g_ranges[range.end] = above;
      }
    }
  }
  it = g_ranges.lower_bound(start);
  while (it != g_ranges.end() && it->first < range.end) {
    if (it->second.end > range.end) {
      Range above = it->second;
      const uint64_t skip = range.end - it->first;
      above.file_offset += above.zlib ? 0 : skip;
      above.out_offset += above.zlib ? skip : 0;
      g_ranges.erase(it);
      g_ranges[range.end] = above;
      break;
    }
    it = g_ranges.erase(it);
  }
  g_ranges[start] = range;
}

const Range *Find(Key address, Key &start) {
  auto it = g_ranges.upper_bound(address);
  if (it == g_ranges.begin())
    return nullptr;
  --it;
  if (address >= it->second.end)
    return nullptr;
  start = it->first;
  return &it->second;
}

void OnRead(const std::string &path, uint64_t offset, uint32_t buffer, uint32_t bytes) {
  fnxf_xiii::WpdPatchNoteWrite(buffer, bytes);  // fnxf_xiii (2026-10-25): a language pack's changes (fnxf_xiii_wpd_patch.cpp)
  if (!Tracking())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (fnxf_xiii::LoadWatchOn()) {
    const std::string *p = Intern(path);
    RecordWrite('r', buffer, bytes, p, offset, 0);
    // fnxf_xiii (2026-11-16): each whole page's hash as read (checked again before an inflate reads it).
    const auto *host = reinterpret_cast<const uint8_t *>(REX_KERNEL_MEMORY()->TranslateVirtual(buffer));
    if (host && g_read_pages.size() < 200000) {
      for (uint32_t at = (4096 - (buffer & 4095)) & 4095; at + 4096 <= bytes; at += 4096) {
        PageRead &pr = g_read_pages[buffer + at];
        pr.hash = XXH3_64bits(host + at, 4096);
        pr.ns = NowNsLw();
        pr.file_offset = offset + at;
        pr.path = p;
      }
    }
  }
  Range r;
  const Key start = FromVirtual(buffer);
  r.end = start + bytes;
  r.path = Intern(path);
  r.file_offset = offset;
  Insert(start, r);
}

// fnxf_xiii (2026-11-16, fnxf_xiii_diag_load_watch): a page of a scatter read (no language pack check: as before).
void OnScatter(const std::string &path, uint64_t offset, uint32_t buffer, uint32_t bytes) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string *p = Intern(path);
  RecordWrite('s', buffer, bytes, p, offset, 0);
  Range r;
  const Key start = FromVirtual(buffer);
  r.end = start + bytes;
  r.path = p;
  r.file_offset = offset;
  Insert(start, r);
}

bool Tracking() { return fnxf_xiii::TextureSourcesOn() || fnxf_xiii::LoadWatchOn(); }

// fnxf_xiii (2026-11-16): the recorded writes into [lo, hi), newest first (at most 'max' listed; 'total' counts all).
std::string ListWrites(Key lo, Key hi, uint64_t now, uint32_t max, uint32_t &total, uint32_t &shown) {
  std::string lines;
  total = shown = 0;
  const uint64_t n = std::min<uint64_t>(g_write_count, kWrites);
  for (uint64_t k = 0; k < n; ++k) {
    const Write &w = g_writes[(g_write_count - 1 - k) % kWrites];
    if (w.start >= hi || w.start + w.bytes <= lo)
      continue;
    ++total;
    if (shown >= max)
      continue;
    ++shown;
    const int64_t rel = int64_t(w.start) - int64_t(lo);
    lines += fmt::format("\n    {:c} {:.1f} ms ago thread {:08X} at +{:X} ({:+X}) size {:X} (va {:08X}) <- ", w.kind,
                         double(now - w.ns) / 1e6, w.thread, rel < 0 ? 0 : rel, rel, w.bytes, w.guest_va);
    if (w.kind == 'z')
      lines += fmt::format("zlib stream {:08X} out +{:X}{}{}", w.strm, w.offset, w.path ? " of " : "",
                           w.path ? *w.path : std::string());
    else if (w.kind == 'f')
      lines += fmt::format("memset {:02X} by lr {:08X}", w.src & 0xFF, w.lr);
    else if (w.kind == 'c' || w.kind == 'm' || w.kind == 'M')
      lines += fmt::format("copy from {:08X} by lr {:08X}{}{}", w.src, w.lr, w.path ? " = " : "",
                           w.path ? fmt::format("{} @{:X}", *w.path, w.offset) : std::string());
    else
      lines += fmt::format("{} @{:X}", w.path ? *w.path : std::string("?"), w.offset);
  }
  return lines;
}

// fnxf_xiii (2026-11-16): a native memcpy / memmove / memset of 4 KB or more; a copy's source found in the records is
// kept as its file and offset.
void OnBigWrite(char kind, uint32_t d, uint32_t s, uint32_t n, uint32_t lr) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string *path = nullptr;
  uint64_t offset = 0;
  if (kind != 'f') {
    Key start = 0;
    if (const Range *src = Find(FromVirtual(s), start); src && !src->zlib) {
      path = src->path;
      offset = src->file_offset + (FromVirtual(s) - start);
      Range r;  // the copy now holds those bytes of the file
      const Key out = FromVirtual(d);
      r.end = out + n;
      r.path = src->path;
      r.file_offset = offset;
      Insert(out, r);
    }
  }
  // fnxf_xiii (2026-11-16): a copy of 64 KB or more whose source holds 4 KB of 0xCC (from any 1 KB step): its source's
  // recorded writers, at once (the damaged loading pictures are copied from a staging buffer in 128 KB pieces).
  if (false && kind != 'f' && n >= 0x10000) {  // off (2026-11-16): the archive's own data has 4 KB 0xCC runs
    const auto *src = reinterpret_cast<const uint8_t *>(REX_KERNEL_MEMORY()->TranslateVirtual(s));
    uint32_t cc_at = UINT32_MAX;
    for (uint32_t at = 0; src && at + 4096 <= n && cc_at == UINT32_MAX; at += 1024) {
      bool all = true;
      for (uint32_t i = 0; i < 4096 && all; i += 8) {
        uint64_t v;
        std::memcpy(&v, src + at + i, 8);
        all = v == 0xCCCCCCCCCCCCCCCCull;
      }
      if (all)
        cc_at = at;
    }
    static uint32_t cc_logged = 0;
    if (cc_at != UINT32_MAX && cc_logged < 40) {
      ++cc_logged;
      uint32_t total = 0, shown = 0;
      const Key lo = FromVirtual(s);
      const std::string lines = ListWrites(lo, lo + n, NowNsLw(), 40, total, shown);
      REXLOG_INFO("[loadwatch] COPY OF 0xCC: {:c} {:08X} <- {:08X} size {:X} by lr {:08X} thread {:08X}: 4 KB of 0xCC "
                  "at source +{:X}; {} recorded writes into the source ({} shown, newest first){}",
                  kind, d, s, n, lr, ThreadTag(), cc_at, total, shown, lines);
    }
  }
  RecordWrite(kind, d, n, path, offset, 0);
  Write &w = g_writes[(g_write_count - 1) % kWrites];
  w.src = s;
  w.lr = lr;
}

}  // namespace

namespace fnxf_xiii {

bool TextureSourcesOn() {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_texture_sources);
  return on;
}

bool LoadWatchOn() {
  static const bool on = REXCVAR_GET(fnxf_xiii_diag_load_watch);
  return on;
}

// fnxf_xiii (2026-11-16): the large untiled pictures uploaded lately, watched each frame for a change of their first
// 64 KB after the upload (the damaged loading pictures were fine at their upload in the last runs).
struct Watched {
  uint32_t texture_va = 0, base = 0, bytes = 0;
  const uint8_t *data = nullptr;
  uint64_t hash = 0, frame = 0, ns = 0;
  uint32_t cc = 0;
};
std::vector<Watched> g_watched;  // g_mutex

uint64_t Hash64k(const uint8_t *p, uint32_t bytes, uint32_t &cc) {
  uint64_t h = 1469598103934665603ull;
  cc = 0;
  const uint32_t n = std::min<uint32_t>(bytes, 0x10000);
  for (uint32_t i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 1099511628211ull;
    cc += p[i] == 0xCC;
  }
  return h;
}

// native zlib, before an inflate: every whole page of its input that a recorded read filled is hashed again; a
// difference means the input changed between its read and this inflate ([loadwatch] INPUT CHANGED).
void LoadWatchBeforeInflate(uint32_t strm, uint32_t next_in, uint32_t avail_in, uint32_t next_out) {
  if (!LoadWatchOn() || !avail_in)
    return;
  const auto *host = reinterpret_cast<const uint8_t *>(REX_KERNEL_MEMORY()->TranslateVirtual(next_in));
  if (!host)
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  static uint32_t logged = 0;
  for (uint32_t at = (4096 - (next_in & 4095)) & 4095; at + 4096 <= avail_in; at += 4096) {
    auto it = g_read_pages.find(next_in + at);
    if (it == g_read_pages.end())
      continue;
    if (XXH3_64bits(host + at, 4096) != it->second.hash && logged < 60) {
      ++logged;
      uint32_t total = 0, shown = 0;
      const Key lo = FromVirtual(next_in + at);
      const std::string lines = ListWrites(lo, lo + 4096, NowNsLw(), 8, total, shown);
      REXLOG_INFO("[loadwatch] INPUT CHANGED: inflate of stream {:08X} (out to {:08X}) reads page {:08X}, which "
                  "changed since its read {:.1f} ms ago ({} @{:X}); recorded writes into it (newest first){}",
                  strm, next_out, next_in + at, double(NowNsLw() - it->second.ns) / 1e6,
                  it->second.path ? *it->second.path : std::string("?"), it->second.file_offset, lines);
    }
  }
}

void LoadWatchTick(uint64_t frame) {
  if (!LoadWatchOn())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (size_t i = 0; i < g_watched.size();) {
    Watched &w = g_watched[i];
    uint32_t cc = 0;
    const uint64_t h = Hash64k(w.data, w.bytes, cc);
    if (h != w.hash) {
      const uint64_t now = NowNsLw();
      const Key lo = kPhysical | (w.base & 0x1FFFFFFFu);
      uint32_t total = 0, shown = 0;
      const std::string lines = ListWrites(lo, lo + 0x10000, now, 12, total, shown);
      REXLOG_INFO("[loadwatch] CHANGED frame {}: tex {:08X} base {:08X}: its first 64 KB changed {} frames ({:.1f} ms) "
                  "after its upload; 0xCC bytes {} -> {}; {} recorded writes into those 64 KB ({} shown, newest "
                  "first){}",
                  frame, w.texture_va, w.base, frame - w.frame, double(now - w.ns) / 1e6, w.cc, cc, total, shown,
                  lines);
      w.hash = h;
      w.cc = cc;
    }
    if (frame - w.frame > 900) {  // watched for about 30 s
      g_watched.erase(g_watched.begin() + i);
      continue;
    }
    ++i;
  }
}

// render/textures.cpp Upload (fnxf_xiii_diag_load_watch): 'data' is the base level's 'bytes' at physical 'base'.
void LoadWatchCheckUpload(uint32_t texture_va, uint32_t base, uint32_t bytes, uint32_t width, uint32_t height,
                          uint32_t format, bool tiled, uint64_t frame, const uint8_t *data) {
  if (!LoadWatchOn() || !data || !bytes)
    return;
  // 4 KB pages (from the level's start) that are all 0xCC.
  const uint32_t scan = std::min<uint32_t>(bytes, 4u << 20);
  uint32_t bad = 0, first_bad = 0, last_bad = 0;
  for (uint32_t at = 0; at + 4096 <= scan; at += 4096) {
    bool all = true;
    for (uint32_t i = 0; i < 4096 && all; i += 8) {
      uint64_t v;
      std::memcpy(&v, data + at + i, 8);
      all = v == 0xCCCCCCCCCCCCCCCCull;
    }
    if (all) {
      if (!bad++)
        first_bad = at;
      last_bad = at;
    }
  }
  // fnxf_xiii (2026-11-16): 0xCC bytes in the first 64 KB (a band that is not whole pages shows here).
  uint32_t cc64 = 0;
  for (uint32_t i = 0; i < std::min<uint32_t>(bytes, 0x10000); ++i)
    cc64 += data[i] == 0xCC;
  static uint32_t ok_logged = 0;
  const bool large = !tiled && width >= 1024 && height >= 512;
  if (large) {  // fnxf_xiii (2026-11-16): watched from now on (LoadWatchTick)
    std::lock_guard<std::mutex> lock(g_mutex);
    bool known = false;
    for (Watched &w : g_watched)
      if (w.base == base) {
        known = true;
        w.texture_va = texture_va;
        w.hash = Hash64k(data, bytes, w.cc);
        w.frame = frame;
        w.ns = NowNsLw();
      }
    if (!known && g_watched.size() < 8) {
      Watched w;
      w.texture_va = texture_va;
      w.base = base;
      w.bytes = bytes;
      w.data = data;
      w.hash = Hash64k(data, bytes, w.cc);
      w.frame = frame;
      w.ns = NowNsLw();
      g_watched.push_back(w);
    }
  }
  // fnxf_xiii (2026-11-16): small textures with an all-0xCC page past their first (their padding) are not news.
  const bool damaged = bad && (large || first_bad == 0);
  if (!damaged && !(large && ok_logged < 40))
    return;
  bad = damaged ? bad : 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  static std::set<std::pair<uint32_t, uint32_t>> logged;
  if (!logged.insert({texture_va, base}).second && !bad) {
    // fnxf_xiii (2026-11-16): a large picture's later uploads, one short line each (which ones reach the GPU).
    static uint32_t again = 0;
    if (large && again++ < 300)
      REXLOG_INFO("[loadwatch] upload again frame {} tex {:08X} base {:08X}: {} bytes 0xCC in the first 64 KB", frame,
                  texture_va, base, cc64);
    return;
  }
  if (!bad)
    ++ok_logged;
  const uint64_t now = NowNsLw();
  const Key lo = kPhysical | (base & 0x1FFFFFFFu), hi = lo + bytes;
  uint32_t shown = 0, total = 0;
  std::string lines = ListWrites(lo, hi, now, 48, total, shown);
  // fnxf_xiii (2026-11-16): each copy into it (newest first, at most 12) with the last recorded write into its source
  // before it: which zlib output (stream offset) or read the copied bytes came from.
  {
    const uint64_t count = std::min<uint64_t>(g_write_count, kWrites);
    uint32_t copies = 0;
    for (uint64_t k = 0; k < count && copies < 12; ++k) {
      const Write &c = g_writes[(g_write_count - 1 - k) % kWrites];
      if (c.start >= hi || c.start + c.bytes <= lo || (c.kind != 'c' && c.kind != 'm' && c.kind != 'M'))
        continue;
      ++copies;
      const Key slo = FromVirtual(c.src), shi = slo + c.bytes;
      lines += fmt::format("\n    copy {:.1f} ms ago to +{:X} from {:08X} size {:X}:", double(now - c.ns) / 1e6,
                           int64_t(c.start) - int64_t(lo), c.src, c.bytes);
      uint32_t fills = 0;
      for (uint64_t j = k + 1; j < count && fills < 3; ++j) {
        const Write &w = g_writes[(g_write_count - 1 - j) % kWrites];
        if (w.start >= shi || w.start + w.bytes <= slo)
          continue;
        ++fills;
        lines += fmt::format(" [{:c} {:.1f} ms before, at src +{:X} size {:X}", w.kind, double(c.ns - w.ns) / 1e6,
                             int64_t(w.start) - int64_t(slo), w.bytes);
        if (w.kind == 'z')
        {
          lines += fmt::format(" zlib {:08X} out +{:X} from input {:08X} size {:X}{}", w.strm, w.offset, w.src, w.lr,
                               w.path ? " of " + *w.path : std::string());
          // fnxf_xiii (2026-11-16): the reads that filled that input before it (newest first, at most 2).
          const Key ilo = FromVirtual(w.src), ihi = ilo + std::max<uint32_t>(w.lr, 1);
          uint32_t reads = 0;
          for (uint64_t i = j + 1; i < count && reads < 2; ++i) {
            const Write &r = g_writes[(g_write_count - 1 - i) % kWrites];
            if (r.start >= ihi || r.start + r.bytes <= ilo || (r.kind != 'r' && r.kind != 's' && r.kind != 'c'))
              continue;
            ++reads;
            lines += fmt::format(" <{:c} {:.1f} ms before thread {:08X} at in +{:X} size {:X} {}{:X}>", r.kind,
                                 double(w.ns - r.ns) / 1e6, r.thread, int64_t(r.start) - int64_t(ilo), r.bytes,
                                 r.path ? *r.path + " @" : std::string("from "), r.path ? r.offset : r.src);
          }
          lines += "]";
        }
        else
          lines += fmt::format(" {} @{:X}]", w.path ? *w.path : std::string("?"), w.offset);
        if (w.start <= slo && w.start + w.bytes >= shi)
          break;  // it covered the whole source
      }
    }
  }
  REXLOG_INFO("[loadwatch] {} frame {} tex {:08X} {}x{} fmt {}{} base {:08X} size {:X}: {} pages all 0xCC{}, {} bytes "
              "0xCC in the first 64 KB; {} recorded writes into it ({} shown, newest first; {} reads and {} copies / "
              "fills recorded so far){}",
              bad ? "DAMAGED" : "ok", frame, texture_va, width, height, format, tiled ? " tiled" : "", base, bytes,
              bad, bad ? fmt::format(" (+{:X} .. +{:X})", first_bad, last_bad + 0xFFF) : std::string(), cc64, total,
              shown, g_reads_seen, g_crt_seen, lines);
}

void TextureSourcesInit() {
  // fnxf_xiii (2026-10-25): the game's file reads are also watched for a language pack's changes (fnxf_xiii_wpd_patch.cpp).
  if (TextureSourcesOn() || WpdPatchActive() || LoadWatchOn())
    rex::kernel::xboxkrnl::g_read_observer.store(&OnRead, std::memory_order_release);
  if (LoadWatchOn()) {  // fnxf_xiii (2026-11-16)
    rex::kernel::xboxkrnl::g_scatter_observer.store(&OnScatter, std::memory_order_release);
    fnxf_xiii::native_crt::g_big_write_observer.store(&OnBigWrite, std::memory_order_release);
    REXLOG_INFO("[loadwatch] on: file reads and zlib outputs remembered; damaged textures logged at upload");
  }
  if (!TextureSourcesOn())
    return;
  REXLOG_INFO("[texsrc] on: game file reads, zlib streams and Renut's texture uploads are recorded");
}

// fnxf_xiii_native_zlib.cpp, after each inflate call: 'used_in' bytes at 'next_in' gave 'made_out' bytes at 'next_out';
// 'total_in' and 'total_out' are the stream's counts before the call.
void TextureSourcesNoteInflate(uint32_t strm, uint32_t next_in, uint32_t used_in, uint32_t total_in, uint32_t next_out,
                               uint32_t made_out, uint32_t total_out) {
  if (!Tracking() || !made_out)
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Stream &stream = g_streams[strm];
  if (total_in == 0 || !stream.path) {
    stream = {};
    const Key in = FromVirtual(next_in);
    Key start = 0;
    if (const Range *src = Find(in, start); src && !src->zlib) {
      stream.path = src->path;
      stream.start = src->file_offset + (in - start) - total_in;
    }
  }
  if (LoadWatchOn())  // fnxf_xiii (2026-11-16, fnxf_xiii_diag_load_watch)
  {
    RecordWrite('z', next_out, made_out, stream.path, total_out, strm);
    Write &w = g_writes[(g_write_count - 1) % kWrites];
    w.src = next_in;  // fnxf_xiii (2026-11-16): the input it used ('lr' holds its length)
    w.lr = used_in;
  }
  if (!stream.path)
    return;
  Range r;
  const Key out = FromVirtual(next_out);
  r.end = out + made_out;
  r.path = stream.path;
  r.file_offset = stream.start;
  r.zlib = true;
  r.out_offset = total_out;
  Insert(out, r);
}

// fnxf_xiii_native_zlib.cpp, at inflateInit / inflateEnd: the stream's source is found again at its next first call.
void TextureSourcesForgetStream(uint32_t strm) {
  if (!Tracking())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_streams.erase(strm);
}

// render/textures.cpp Upload: texture header 'texture_va', its base level's data at physical address 'base' ('bytes'
// long).
void TextureSourcesNoteUpload(uint32_t texture_va, uint32_t base, uint32_t bytes, uint32_t width, uint32_t height,
                              uint32_t depth, uint32_t format, uint32_t mip_levels, bool tiled, uint64_t frame) {
  if (!TextureSourcesOn())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_logged.insert({texture_va, base, bytes, format}).second)
    return;
  if (g_logged.size() > 200000)
    return;
  ++g_textures_logged;
  const Key key = kPhysical | (base & 0x1FFFFFFFu);
  Key start = 0;
  const Range *src = Find(key, start);
  const uint64_t in_range = src ? key - start : 0;
  const bool whole = src && key + bytes <= src->end;
  if (!src) {
    ++g_textures_unknown;
    REXLOG_INFO("[texsrc] frame {} tex {:08X} {}x{}x{} fmt {} mips {}{} base {:08X} size {:X} <- ?", frame, texture_va,
                width, height, depth, format, mip_levels, tiled ? " tiled" : "", base, bytes);
  } else if (src->zlib) {
    REXLOG_INFO("[texsrc] frame {} tex {:08X} {}x{}x{} fmt {} mips {}{} base {:08X} size {:X} <- {} zlib stream @{:X} "
                "+{:X}{}",
                frame, texture_va, width, height, depth, format, mip_levels, tiled ? " tiled" : "", base, bytes,
                *src->path, src->file_offset, src->out_offset + in_range, whole ? "" : " (partly)");
  } else {
    REXLOG_INFO("[texsrc] frame {} tex {:08X} {}x{}x{} fmt {} mips {}{} base {:08X} size {:X} <- {} @{:X}{}", frame,
                texture_va, width, height, depth, format, mip_levels, tiled ? " tiled" : "", base, bytes, *src->path,
                src->file_offset + in_range, whole ? "" : " (partly)");
  }
  if (g_textures_logged % 500 == 0)
    REXLOG_INFO("[texsrc] {} textures logged, {} of them from no recorded file data", g_textures_logged,
                g_textures_unknown);
}

}  // namespace fnxf_xiii
