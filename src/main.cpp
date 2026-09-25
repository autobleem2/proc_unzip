//
// proc_unzip - an AutoBleem scanner processor that unpacks PS1 games and ROMs from .zip, .7z and .rar.
//
// It is also the example to copy when writing a processor of your own. A processor is an ordinary console
// program that lives in System/Processors/<name>/ on the stick; the launcher's scan starts it and reads what
// it prints. Everything it has to know is in this file:
//
//   unzip --version                                  "#Unzip V1.1.0 - <what it does>"
//   unzip --ismine --rom <file> --system "<name>"     exit 0 = mine, 1 = not mine
//   unzip --start --games <Games dir>                the whole PS1 tree, first thing in every scan
//   unzip --start --rom <file> --system "<name>"      one ROM file
//
// What it prints while it works (one line at a time, flushed):
//
//   #Starting - Unzip V1.1.0      the first line; the bubble's title
//   #Unpacking Crash.7z           a new stage
//   1/3                           which item of how many (optional)
//   0 .. 100                      percent of the current stage
//   #WARN - <text>                a warning; the job goes on
//   #DONE                         finished - and exit 0
//   #ERROR - <text>               failed - and exit 1
//
// The rules it keeps, which every processor must keep:
//   - atomic: every file is written as <name>.part and renamed only when complete; the archive is deleted only
//     after the last rename. It can be killed at any moment (a game starting, the power button) and started
//     again on the same input, so it deletes a stale <name>.part before writing one.
//   - idempotent: run again on its own output it finds nothing to do and prints #DONE.
//   - it stays inside what it was given, and never overwrites a file that is already there.
//   - small scratch files only in $AB_TMP (off the stick; RAM on the console). This one needs none.
//
// docs/scanner-processors-plan.md in github.com/autobleem2/autobleem is the whole protocol.
//
// The archives: a .zip through miniz; a .7z or a .rar (RAR 1.5-4.x and RAR5, a multi-volume set too) through
// libarchive, which reads them as a stream - a solid 7z is never held in memory, which matters on a console
// with 512 MB. Both sit behind one small Archive class, so the work below does not care which it has.
//
#include "miniz.h"

#include "ab_names.h"
#include "archive.h"
#include "archive_entry.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

const char *Version = "1.1.0";
const char *Description = "Unpacks PS1 games and ROMs from .zip, .7z and .rar";

//******************
// the protocol's output
//******************
// every line is flushed at once: the launcher shows progress as it comes, and a pipe is block-buffered
void say(const string &line) {
    fputs(line.c_str(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

//******************
// a little portable filesystem (UTF-8 paths on every system)
//******************
#ifdef _WIN32
wstring wide(const string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
string narrow(const wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
#endif

struct Entry {
    string name;
    bool isDir;
};

vector<Entry> listDir(const string &dir) {
    vector<Entry> out;
#ifdef _WIN32
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(wide(dir + "/*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        string name = narrow(data.cFileName);
        if (name != "." && name != "..")
            out.push_back({name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(dir.c_str());
    if (!d)
        return out;
    while (dirent *e = readdir(d)) {
        string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        struct stat st;
        bool isDir = stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        out.push_back({name, isDir});
    }
    closedir(d);
#endif
    return out;
}

bool exists(const string &path) {
#ifdef _WIN32
    return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

bool isDir(const string &path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool makeDirs(const string &path) {
    if (path.empty() || isDir(path))
        return true;
    size_t slash = path.find_last_of('/');
    if (slash != string::npos && slash > 0 && !makeDirs(path.substr(0, slash)))
        return false;
#ifdef _WIN32
    return CreateDirectoryW(wide(path).c_str(), nullptr) || isDir(path);
#else
    return mkdir(path.c_str(), 0777) == 0 || isDir(path);
#endif
}

bool removeFile(const string &path) {
#ifdef _WIN32
    return DeleteFileW(wide(path).c_str()) != 0;
#else
    return unlink(path.c_str()) == 0;
#endif
}

bool renameFile(const string &from, const string &to) {
#ifdef _WIN32
    return MoveFileW(wide(from).c_str(), wide(to).c_str()) != 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}

FILE *openFile(const string &path, const char *mode) {
#ifdef _WIN32
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

// 64-bit seeks on every target: a disc image is past 2 GB now and then
int seekFile(FILE *f, int64_t offset, int whence) {
#ifdef _WIN32
    return _fseeki64(f, offset, whence);
#else
    return fseeko(f, static_cast<off_t>(offset), whence);
#endif
}

int64_t tellFile(FILE *f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return static_cast<int64_t>(ftello(f));
#endif
}

// the size of a file; -1 when it cannot be read
int64_t fileSize(const string &path) {
    FILE *f = openFile(path, "rb");
    if (!f)
        return -1;
    int64_t size = seekFile(f, 0, SEEK_END) == 0 ? tellFile(f) : -1;
    fclose(f);
    return size;
}

// bytes free on the filesystem `path` is on; UINT64_MAX when it cannot be told
uint64_t freeSpace(const string &path) {
#ifdef _WIN32
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExW(wide(path).c_str(), &avail, nullptr, nullptr))
        return avail.QuadPart;
#else
    struct statvfs fs;
    if (statvfs(path.c_str(), &fs) == 0)
        return static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
#endif
    return UINT64_MAX;
}

string lower(string s) {
    for (char &c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const string &s, const string &suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == suffix;
}

string fileName(const string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == string::npos ? path : path.substr(slash + 1);
}

string dirName(const string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == string::npos ? "." : path.substr(0, slash);
}

string withoutSlash(string path) {
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return path;
}

//******************
// which files make one archive
//******************
// the archives this program reads, by extension
bool isArchiveName(const string &name) {
    return endsWith(name, ".zip") || endsWith(name, ".7z") || endsWith(name, ".rar");
}

// "Game.part07.rar" -> "Game", 7 and 2 (the digits it is written with); false for any other name
bool rarPart(const string &name, string &stem, int &number, size_t &digits) {
    if (!endsWith(name, ".rar"))
        return false;
    size_t end = name.size() - 4;
    size_t start = end;
    while (start > 0 && isdigit(static_cast<unsigned char>(name[start - 1])))
        --start;
    if (start == end || start < 5 || lower(name.substr(start - 5, 5)) != ".part")
        return false;
    stem = name.substr(0, start - 5);
    number = atoi(name.substr(start, end - start).c_str());
    digits = end - start;
    return true;
}

// Every file the archive at `path` is made of, the first first: itself, or the volumes of a multi-volume
// RAR - "Game.part1.rar", "Game.part2.rar", ... or the older "Game.rar", "Game.r00", "Game.r01", ...
// Empty for a later volume of a set: that one is read, and deleted, with the first.
vector<string> volumesOf(const string &path) {
    string stem;
    int number;
    size_t digits;
    if (rarPart(path, stem, number, digits)) {
        if (number != 1)
            return {};
        vector<string> volumes{path};
        string part = path.substr(stem.size(), 5);     // ".part", spelled as the first volume spells it
        string ext = path.substr(path.size() - 4);     // ".rar", the same
        for (int n = 2;; ++n) {
            string num = to_string(n);
            if (num.size() < digits)
                num = string(digits - num.size(), '0') + num;
            string next = stem + part + num + ext;
            if (!exists(next))
                break;
            volumes.push_back(next);
        }
        return volumes;
    }
    if (endsWith(path, ".rar")) {
        vector<string> volumes{path};
        string base = path.substr(0, path.size() - 4);
        string r = path[path.size() - 3] == 'R' ? ".R" : ".r";
        for (int n = 0; n < 100; ++n) {
            string next = base + r + (n < 10 ? "0" : "") + to_string(n);
            if (!exists(next))
                break;
            volumes.push_back(next);
        }
        return volumes;
    }
    return {path};
}

//******************
// Archive
//******************
// where one entry's bytes go: a new file, or a comparison with a file already there
struct Sink {
    virtual ~Sink() = default;
    virtual bool write(const void *buf, size_t n) = 0;
};

// One archive: its file entries (folders left out) with their unpacked sizes, and a way to stream them.
class Archive {
public:
    struct Item {
        string name;   // as stored, '/' separated
        uint64_t size; // unpacked
        bool hasCrc;   // a zip's directory has each file's CRC; libarchive does not tell it
        uint32_t crc;
    };

    vector<Item> items;
    string error; // why open() or extract() failed; empty after a good open()

    virtual ~Archive() = default;
    virtual bool open(const vector<string> &volumes) = 0;
    // every item's bytes, in the archive's order, to the sink pick(index) gives; nullptr skips the item
    virtual bool extract(const function<Sink *(size_t)> &pick) = 0;
};

// a .zip, through miniz. Opened through our own FILE*, so a path with non-ASCII characters works on Windows too.
class ZipArchive : public Archive {
public:
    ZipArchive() { memset(&zip, 0, sizeof(zip)); }
    ~ZipArchive() override {
        mz_zip_reader_end(&zip);
        if (file)
            fclose(file);
    }

    bool open(const vector<string> &volumes) override {
        file = openFile(volumes[0], "rb");
        if (!file || !mz_zip_reader_init_cfile(&zip, file, 0, 0)) {
            error = "not a zip this program can read";
            return false;
        }
        mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory)
                continue;
            items.push_back({st.m_filename, st.m_uncomp_size, true, st.m_crc32});
            indexes.push_back(i);
        }
        return true;
    }

    bool extract(const function<Sink *(size_t)> &pick) override {
        for (size_t i = 0; i < items.size(); ++i) {
            Sink *sink = pick(i);
            if (!sink)
                continue;
            auto write = [](void *opaque, mz_uint64, const void *buf, size_t n) -> size_t {
                return static_cast<Sink *>(opaque)->write(buf, n) ? n : 0;
            };
            if (!mz_zip_reader_extract_to_callback(&zip, indexes[i], write, sink, 0)) {
                error = "could not unpack " + items[i].name;
                return false;
            }
        }
        return true;
    }

private:
    mz_zip_archive zip;
    FILE *file = nullptr;
    vector<mz_uint> indexes; // each item's index in the zip
};

// A .7z or a .rar (every volume of a set), through libarchive. libarchive reads an archive front to back, so
// open() lists it on one pass and extract() reads it again on another. The volumes reach libarchive through
// callbacks over our own FILE*s, for the same reason as the zip's.
class LibArchive : public Archive {
public:
    bool open(const vector<string> &paths) override {
        for (const string &path : paths)
            volumes.emplace_back(path);
        Reader reader(volumes);
        if (!reader.a) {
            error = reader.why;
            return false;
        }
        archive_entry *entry;
        int r;
        while ((r = archive_read_next_header(reader.a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
            if (archive_entry_filetype(entry) != AE_IFREG)
                continue; // folders; a link would be the odd one out, and is not unpacked
            if (archive_entry_is_encrypted(entry)) {
                error = "it is protected by a password";
                return false;
            }
#ifdef _WIN32
            const char *name = archive_entry_pathname_utf8(entry);
#else
            // UTF-8 already (ab_archive_names_utf8); asking for "UTF-8" would convert it again, from a
            // locale the console does not have, and fail
            const char *name = archive_entry_pathname(entry);
#endif
            if (!name) {
                error = "a name in it cannot be read";
                return false;
            }
            string n = name;
            for (char &c : n) {
                if (c == '\\') // made on Windows by a program that kept Windows' separator
                    c = '/';
            }
            uint64_t size = archive_entry_size_is_set(entry) ? static_cast<uint64_t>(archive_entry_size(entry)) : 0;
            items.push_back({n, size, false, 0});
        }
        if (r != ARCHIVE_EOF) {
            error = reader.failure("it cannot be read");
            return false;
        }
        return true;
    }

    bool extract(const function<Sink *(size_t)> &pick) override {
        Reader reader(volumes);
        if (!reader.a) {
            error = reader.why;
            return false;
        }
        archive_entry *entry;
        size_t index = 0;
        int r;
        while ((r = archive_read_next_header(reader.a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
            if (archive_entry_filetype(entry) != AE_IFREG)
                continue;
            if (index >= items.size())
                break;
            const Item &item = items[index];
            Sink *sink = pick(index++);
            if (!sink)
                continue; // the next header skips it (in a solid 7z that still means decoding it)
            const void *buf;
            size_t n;
            la_int64_t offset;
            while ((r = archive_read_data_block(reader.a, &buf, &n, &offset)) == ARCHIVE_OK) {
                if (!sink->write(buf, n)) {
                    error = "could not unpack " + item.name;
                    return false;
                }
            }
            if (r != ARCHIVE_EOF) {
                error = reader.failure("could not unpack " + item.name);
                return false;
            }
        }
        if (r != ARCHIVE_EOF && r != ARCHIVE_OK && r != ARCHIVE_WARN) {
            error = reader.failure("it cannot be read");
            return false;
        }
        return true;
    }

private:
    struct Volume {
        explicit Volume(const string &p) : path(p) {}
        string path;
        FILE *file = nullptr;
        vector<unsigned char> buffer = vector<unsigned char>(65536);
    };
    vector<Volume> volumes;

    // one pass over the archive: libarchive set up for the three formats, reading every volume
    struct Reader {
        archive *a = nullptr;
        vector<Volume> &volumes;
        string why;

        explicit Reader(vector<Volume> &v) : volumes(v) {
            a = archive_read_new();
            archive_read_support_format_7zip(a);
            archive_read_support_format_rar(a);
            archive_read_support_format_rar5(a);
            ab_archive_names_utf8(a);
            for (Volume &volume : volumes)
                archive_read_append_callback_data(a, &volume);
            archive_read_set_open_callback(a, onOpen);
            archive_read_set_read_callback(a, onRead);
            archive_read_set_skip_callback(a, onSkip);
            archive_read_set_seek_callback(a, onSeek);
            archive_read_set_close_callback(a, onClose);
            archive_read_set_switch_callback(a, onSwitch);
            if (archive_read_open1(a) != ARCHIVE_OK) {
                why = failure("not a 7z or RAR this program can read");
                archive_read_free(a);
                a = nullptr;
            }
        }
        ~Reader() {
            if (a)
                archive_read_free(a);
            for (Volume &volume : volumes) // whatever libarchive left open: Windows will not delete an open file
                onClose(nullptr, &volume);
        }
        // what went wrong, in libarchive's words when it has some
        string failure(const string &what) const {
            const char *detail = a ? archive_error_string(a) : nullptr;
            return detail ? what + " (" + detail + ")" : what;
        }

        static int onOpen(archive *, void *data) {
            Volume *v = static_cast<Volume *>(data);
            if (!v->file)
                v->file = openFile(v->path, "rb");
            return v->file ? ARCHIVE_OK : ARCHIVE_FATAL;
        }
        static la_ssize_t onRead(archive *, void *data, const void **buf) {
            Volume *v = static_cast<Volume *>(data);
            *buf = v->buffer.data();
            size_t n = fread(v->buffer.data(), 1, v->buffer.size(), v->file);
            return ferror(v->file) ? -1 : static_cast<la_ssize_t>(n);
        }
        static la_int64_t onSkip(archive *, void *data, la_int64_t request) {
            Volume *v = static_cast<Volume *>(data);
            return seekFile(v->file, request, SEEK_CUR) == 0 ? request : 0;
        }
        static la_int64_t onSeek(archive *, void *data, la_int64_t offset, int whence) {
            Volume *v = static_cast<Volume *>(data);
            if (seekFile(v->file, offset, whence) != 0)
                return ARCHIVE_FATAL;
            return tellFile(v->file);
        }
        static int onClose(archive *, void *data) {
            Volume *v = static_cast<Volume *>(data);
            if (v->file)
                fclose(v->file);
            v->file = nullptr;
            return ARCHIVE_OK;
        }
        static int onSwitch(archive *a, void *from, void *to) {
            onClose(a, from);
            return onOpen(a, to);
        }
    };
};

// the archive made of `volumes` (see volumesOf), opened and listed; its error says whether that worked
unique_ptr<Archive> openArchive(const vector<string> &volumes) {
    unique_ptr<Archive> archive;
    if (endsWith(volumes[0], ".zip"))
        archive.reset(new ZipArchive);
    else
        archive.reset(new LibArchive);
    archive->open(volumes);
    return archive;
}

// a name in an archive that could write outside the folder it is unpacked to
bool unsafeName(const string &name) {
    if (name.empty() || name[0] == '/' || name.find('\\') != string::npos || name.find(':') != string::npos)
        return true;
    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find('/', start);
        string part = name.substr(start, slash == string::npos ? string::npos : slash - start);
        if (part == "..")
            return true;
        if (slash == string::npos)
            break;
        start = slash + 1;
    }
    return false;
}

// what makes an archive a PS1 game: a disc image (or its cue/playlist) inside
bool isPs1Game(const Archive &archive) {
    static const char *extensions[] = {".cue", ".bin", ".img", ".iso", ".chd", ".pbp", ".ecm", ".m3u", ".ccd"};
    for (const Archive::Item &item : archive.items) {
        for (const char *ext : extensions) {
            if (endsWith(item.name, ext))
                return true;
        }
    }
    return false;
}

// the systems whose ROMs *are* archives - arcade sets are read packed by their cores and must stay so
bool keepsArchives(const string &system) {
    string s = lower(system);
    for (const char *word : {"mame", "fbneo", "fb alpha", "final burn", "arcade", "neo geo", "cps"}) {
        if (s.find(word) != string::npos)
            return true;
    }
    return false;
}

// every entry under one top folder ("Crash (USA)/Crash.cue", ...): the folder is dropped when unpacking
string commonTopFolder(const Archive &archive) {
    string top;
    for (const Archive::Item &item : archive.items) {
        size_t slash = item.name.find('/');
        if (slash == string::npos)
            return "";
        string first = item.name.substr(0, slash + 1);
        if (top.empty())
            top = first;
        else if (first != top)
            return "";
    }
    return top;
}

// the CRC-32 of a file, to compare with a zip entry's
bool fileCrc(const string &path, uint32_t &crc) {
    FILE *f = openFile(path, "rb");
    if (!f)
        return false;
    mz_ulong c = MZ_CRC32_INIT;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        c = mz_crc32(c, buf, n);
    fclose(f);
    crc = static_cast<uint32_t>(c);
    return true;
}

// an entry's bytes into <target>.part
struct FileSink : Sink {
    FILE *out = nullptr;
    function<void(size_t)> progress;
    bool write(const void *buf, size_t n) override {
        if (fwrite(buf, 1, n, out) != n)
            return false;
        progress(n);
        return true;
    }
};

// an entry's bytes against the file of the same size already where it goes: false at the first difference
struct CompareSink : Sink {
    FILE *in = nullptr;
    function<void(size_t)> progress;
    bool differs = false;
    bool write(const void *buf, size_t n) override {
        unsigned char have[65536];
        const unsigned char *want = static_cast<const unsigned char *>(buf);
        while (n > 0) {
            size_t chunk = n < sizeof(have) ? n : sizeof(have);
            if (fread(have, 1, chunk, in) != chunk || memcmp(have, want, chunk) != 0) {
                differs = true;
                return false;
            }
            progress(chunk);
            want += chunk;
            n -= chunk;
        }
        return true;
    }
};

//******************
// unpacking one archive
//******************
// Unpacks every file of the archive into `dest` (the common top folder dropped, `flatten`: every file's own
// name only), each through <name>.part. A file already there that is the entry - a stopped run's work - is
// kept; one that is something else is a clash. The size tells most clashes up front, then a zip's CRC, and
// for a 7z or a RAR (libarchive does not give the CRC) the bytes are compared as they are unpacked. On any
// failure the files this call made are removed again, the archive is kept, `why` says what went wrong and
// false is returned. `onPercent` follows the bytes unpacked. The caller deletes the archive.
bool unpack(Archive &archive, const string &dest, bool flatten, string &why,
            const function<void(uint64_t done, uint64_t total)> &onPercent) {
    string top = flatten ? "" : commonTopFolder(archive);

    enum class Action { Keep, Write, Compare };
    struct Planned {
        Action action = Action::Keep;
        string target;
        string rel; // the target inside dest, for the messages
    };
    vector<Planned> plan(archive.items.size());
    uint64_t toWrite = 0, toRead = 0;
    for (size_t i = 0; i < archive.items.size(); ++i) {
        const Archive::Item &item = archive.items[i];
        if (unsafeName(item.name)) {
            why = "unsafe name in the archive: " + item.name;
            return false;
        }
        Planned &p = plan[i];
        p.rel = flatten ? fileName(item.name) : item.name.substr(top.size());
        p.target = dest + "/" + p.rel;
        removeFile(p.target + ".part"); // a run that was stopped left it: ours, whatever happens next
        if (!exists(p.target)) {
            p.action = Action::Write;
            toWrite += item.size;
            toRead += item.size;
            continue;
        }
        uint32_t crc;
        if (isDir(p.target) || fileSize(p.target) != static_cast<int64_t>(item.size) ||
            (item.hasCrc && (!fileCrc(p.target, crc) || crc != item.crc))) {
            why = p.rel + " is already there";
            return false;
        }
        if (!item.hasCrc) { // unpacked by a run that was stopped - if the bytes agree
            p.action = Action::Compare;
            toRead += item.size;
        }
    }
    if (!makeDirs(dest)) {
        why = "could not create " + dest;
        return false;
    }
    uint64_t free = freeSpace(dest);
    if (free != UINT64_MAX && toWrite > free) {
        why = "not enough space: " + to_string(toWrite / (1024 * 1024)) + " MB needed, " +
              to_string(free / (1024 * 1024)) + " MB free";
        return false;
    }

    uint64_t done = 0;
    auto progress = [&](size_t n) {
        done += n;
        onPercent(done, toRead);
    };

    // The entry being unpacked is opened when the archive reaches it (pick) and finished - closed, and a
    // written one renamed into place - when it reaches the next, or at the end.
    vector<string> made; // renamed into place: removed again if a later file fails
    FILE *current = nullptr;
    const Planned *currentPlan = nullptr;
    FileSink fileSink;
    CompareSink compareSink;
    fileSink.progress = progress;
    compareSink.progress = progress;
    string failure;
    auto finish = [&]() {
        if (!current)
            return true;
        bool closed = fclose(current) == 0;
        current = nullptr;
        if (currentPlan->action == Action::Compare)
            return true;
        string part = currentPlan->target + ".part";
        if (!closed || !renameFile(part, currentPlan->target)) {
            removeFile(part);
            failure = "could not write " + part;
            return false;
        }
        made.push_back(currentPlan->target);
        return true;
    };
    auto pick = [&](size_t index) -> Sink * {
        if (!failure.empty() || !finish())
            return nullptr; // something failed: the rest is skipped
        currentPlan = &plan[index];
        if (currentPlan->action == Action::Write) {
            makeDirs(dirName(currentPlan->target));
            current = openFile(currentPlan->target + ".part", "wb");
            if (!current) {
                failure = "could not write " + currentPlan->target + ".part";
                return nullptr;
            }
            fileSink.out = current;
            return &fileSink;
        }
        if (currentPlan->action == Action::Compare) {
            current = openFile(currentPlan->target, "rb");
            if (!current) {
                failure = "could not read " + currentPlan->target;
                return nullptr;
            }
            compareSink.in = current;
            compareSink.differs = false;
            return &compareSink;
        }
        return nullptr;
    };

    bool ok = archive.extract(pick) && failure.empty() && finish();
    if (!ok) {
        if (current) { // the entry that failed: its .part goes; a compared file stays as it was
            fclose(current);
            current = nullptr;
            if (currentPlan->action == Action::Write)
                removeFile(currentPlan->target + ".part");
        }
        if (compareSink.differs)
            why = currentPlan->rel + " is already there";
        else if (!failure.empty())
            why = failure;
        else
            why = archive.error + " - a damaged archive, or the stick is full";
        for (const string &path : made)
            removeFile(path);
        return false;
    }
    return true;
}

// the percent line, printed only when it moves
struct PercentLine {
    int last = -1;
    void operator()(uint64_t done, uint64_t total) {
        int percent = total == 0 ? 100 : static_cast<int>(done * 100 / total);
        if (percent != last) {
            last = percent;
            say(to_string(percent));
        }
    }
};

// deletes every volume of an unpacked archive; a warning for one that stays
void removeArchive(const vector<string> &volumes) {
    for (const string &v : volumes) {
        if (!removeFile(v))
            say("#WARN - " + fileName(v) + " was unpacked but could not be deleted");
    }
}

//******************
// --start --games <dir>
//******************
// every archive under the games tree that holds a PS1 game: one loose in Games/ gets a folder named after it,
// one already in a folder is unpacked where it is. An archive that is not a game is left alone (not ours).
void findArchives(const string &dir, vector<vector<string>> &out) {
    for (const Entry &e : listDir(dir)) {
        if (e.name.empty() || e.name[0] == '.' || e.name[0] == '!') // !SaveStates, !MemCards
            continue;
        string path = dir + "/" + e.name;
        if (e.isDir) {
            findArchives(path, out);
        } else if (isArchiveName(e.name)) {
            vector<string> volumes = volumesOf(path);
            if (!volumes.empty())
                out.push_back(volumes);
        }
    }
}

// the folder a loose archive's game gets: "Crash (USA).7z", "Crash (USA).part1.rar" -> "Crash (USA)"
string archiveStem(const string &path) {
    string name = fileName(path);
    string stem;
    int number;
    size_t digits;
    if (rarPart(name, stem, number, digits))
        return stem;
    return name.substr(0, name.find_last_of('.'));
}

int startGames(const string &gamesDir) {
    vector<vector<string>> archives;
    findArchives(gamesDir, archives);

    vector<vector<string>> games;
    for (const vector<string> &volumes : archives) {
        unique_ptr<Archive> archive = openArchive(volumes);
        if (archive->error.empty() && isPs1Game(*archive))
            games.push_back(volumes);
    }

    int failed = 0;
    string firstReason;
    for (size_t i = 0; i < games.size(); ++i) {
        const vector<string> &volumes = games[i];
        const string &first = volumes[0];
        say("#Unpacking " + fileName(first));
        say(to_string(i + 1) + "/" + to_string(games.size()));
        string why;
        bool ok = false;
        { // the archive is closed at the end of this block: Windows will not delete an open file
            unique_ptr<Archive> archive = openArchive(volumes);
            if (archive->error.empty()) {
                string parent = dirName(first);
                string dest = withoutSlash(parent) == gamesDir ? parent + "/" + archiveStem(first) : parent;
                ok = unpack(*archive, dest, false, why, PercentLine());
            } else {
                why = archive->error;
            }
        }
        if (ok) {
            removeArchive(volumes);
        } else {
            ++failed;
            if (firstReason.empty())
                firstReason = fileName(first) + ": " + why;
            say("#WARN - " + fileName(first) + ": " + why);
        }
    }
    if (failed > 0) {
        say("#ERROR - " + to_string(failed) + " of " + to_string(games.size()) +
            " archives could not be unpacked (" + firstReason + ")");
        return 1;
    }
    say("#DONE");
    return 0;
}

//******************
// --ismine / --start --rom <file> --system <name>
//******************
// one file inside and not an arcade system: the ROM itself, next to where the archive was. A later volume of
// a RAR set is not a ROM of its own - its first volume is.
bool romIsMine(const string &file, const string &system) {
    if (!isArchiveName(file) || keepsArchives(system))
        return false;
    vector<string> volumes = volumesOf(file);
    if (volumes.empty())
        return false;
    unique_ptr<Archive> archive = openArchive(volumes);
    return archive->error.empty() && archive->items.size() == 1;
}

int startRom(const string &file, const string &system) {
    if (!romIsMine(file, system)) {
        say("#DONE"); // not ours: nothing to do is a success
        return 0;
    }
    say("#Unpacking " + fileName(file));
    vector<string> volumes = volumesOf(file);
    string why;
    bool ok;
    {
        unique_ptr<Archive> archive = openArchive(volumes);
        ok = archive->error.empty() && unpack(*archive, dirName(file), true, why, PercentLine());
        if (why.empty())
            why = archive->error;
    }
    if (!ok) {
        say("#ERROR - " + fileName(file) + ": " + why);
        return 1;
    }
    removeArchive(volumes);
    say("#DONE");
    return 0;
}

int usage() {
    fprintf(stderr, "usage: unzip --version | --ismine --rom <file> --system <name> | --start --games <dir>"
                    " | --start --rom <file> --system <name>\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    vector<string> args(argv + 1, argv + argc);
    auto value = [&args](const string &flag) {
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == flag)
                return withoutSlash(args[i + 1]);
        }
        return string();
    };
    auto has = [&args](const string &flag) {
        for (const string &a : args) {
            if (a == flag)
                return true;
        }
        return false;
    };

    const string title = string("Unzip V") + Version;
    if (has("--version")) {
        say("#" + title + " - " + Description);
        return 0;
    }
    if (has("--ismine")) {
        if (has("--rom"))
            return romIsMine(value("--rom"), value("--system")) ? 0 : 1;
        return 1; // this one is a games-folder and rom processor; anything else is not its business
    }
    if (has("--start")) {
        say("#Starting - " + title);
        if (has("--games"))
            return startGames(value("--games"));
        if (has("--rom"))
            return startRom(value("--rom"), value("--system"));
        say("#DONE"); // a kind it does not do: nothing to do
        return 0;
    }
    return usage();
}
