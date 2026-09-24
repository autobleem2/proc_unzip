//
// proc_unzip - an AutoBleem scanner processor that unpacks zipped PS1 games and ROMs.
//
// It is also the example to copy when writing a processor of your own. A processor is an ordinary console
// program that lives in System/Processors/<name>/ on the stick; the launcher's scan starts it and reads what
// it prints. Everything it has to know is in this file:
//
//   unzip --version                                  "#Unzip V1.0.0 - <what it does>"
//   unzip --ismine --rom <file> --system "<name>"     exit 0 = mine, 1 = not mine
//   unzip --start --games <Games dir>                the whole PS1 tree, first thing in every scan
//   unzip --start --rom <file> --system "<name>"      one ROM file
//
// What it prints while it works (one line at a time, flushed):
//
//   #Starting - Unzip V1.0.0      the first line; the bubble's title
//   #Unpacking Crash.zip          a new stage
//   1/3                           which item of how many (optional)
//   0 .. 100                      percent of the current stage
//   #WARN - <text>                a warning; the job goes on
//   #DONE                         finished - and exit 0
//   #ERROR - <text>               failed - and exit 1
//
// The rules it keeps, which every processor must keep:
//   - atomic: every file is written as <name>.part and renamed only when complete; the zip is deleted only
//     after the last rename. It can be killed at any moment (a game starting, the power button) and started
//     again on the same input, so it deletes a stale <name>.part before writing one.
//   - idempotent: run again on its own output it finds nothing to do and prints #DONE.
//   - it stays inside what it was given, and never overwrites a file that is already there.
//   - small scratch files only in $AB_TMP (off the stick; RAM on the console). This one needs none.
//
// docs/scanner-processors-plan.md in github.com/autobleem2/autobleem is the whole protocol.
//
#include "miniz.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

const char *Version = "1.0.0";
const char *Description = "Unpacks zipped PS1 games and ROMs";

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
// Archive
//******************
// one zip: its file entries (folders left out) and their unpacked sizes. Opened through our own FILE*, so a
// path with non-ASCII characters works on Windows too.
struct Archive {
    struct Item {
        mz_uint index;
        string name; // as stored, '/' separated
        uint64_t size;
        mz_uint32 crc; // of the unpacked bytes, from the zip's directory
    };

    mz_zip_archive zip;
    FILE *file = nullptr;
    vector<Item> items;
    uint64_t totalSize = 0;

    Archive() { memset(&zip, 0, sizeof(zip)); }
    ~Archive() {
        mz_zip_reader_end(&zip);
        if (file)
            fclose(file);
    }

    bool open(const string &path) {
        file = openFile(path, "rb");
        if (!file || !mz_zip_reader_init_cfile(&zip, file, 0, 0))
            return false;
        mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory)
                continue;
            items.push_back({i, st.m_filename, st.m_uncomp_size, st.m_crc32});
            totalSize += st.m_uncomp_size;
        }
        return true;
    }
};

// a name in a zip that could write outside the folder it is unpacked to
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

// what makes a zip a PS1 game: a disc image (or its cue/playlist) inside
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

// the systems whose ROMs *are* zips - arcade sets are read zipped by their cores and must stay so
bool keepsZips(const string &system) {
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

// a file already where an entry goes, with the entry's size and CRC: this program put it there on a run
// that was stopped before the zip was finished (the rest is what a restart does), so it is kept, not a clash
bool sameAsEntry(const string &path, const Archive::Item &item) {
    FILE *f = openFile(path, "rb");
    if (!f)
        return false;
    mz_ulong crc = MZ_CRC32_INIT;
    uint64_t size = 0;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = mz_crc32(crc, buf, n);
        size += n;
    }
    fclose(f);
    return size == item.size && static_cast<mz_uint32>(crc) == item.crc;
}

//******************
// unpacking one zip
//******************
// Unpacks every file of the zip into `dest` (the common top folder dropped, `flatten`: every file's own
// name only), each through <name>.part. A file already there that is the entry (a stopped run's work) is
// kept; one that is something else is a clash. On any failure the files this call made are removed again,
// the zip is kept, `why` says what went wrong and false is returned. `onPercent` follows the bytes written.
// The caller deletes the zip.
template <typename Progress>
bool unpack(Archive &archive, const string &dest, bool flatten, string &why,
            Progress onPercent) {
    string top = flatten ? "" : commonTopFolder(archive);

    struct Planned {
        const Archive::Item *item;
        string target;
    };
    vector<Planned> plan;
    for (const Archive::Item &item : archive.items) {
        if (unsafeName(item.name)) {
            why = "unsafe name in the zip: " + item.name;
            return false;
        }
        string rel = flatten ? fileName(item.name) : item.name.substr(top.size());
        string target = dest + "/" + rel;
        removeFile(target + ".part"); // a run that was stopped left it: ours, whatever happens next
        if (exists(target)) {
            if (isDir(target) || !sameAsEntry(target, item)) {
                why = rel + " is already there";
                return false;
            }
            continue; // unpacked by a run that was stopped
        }
        plan.push_back({&item, target});
    }
    if (!makeDirs(dest)) {
        why = "could not create " + dest;
        return false;
    }
    uint64_t free = freeSpace(dest);
    if (free != UINT64_MAX && archive.totalSize > free) {
        why = "not enough space: " + to_string(archive.totalSize / (1024 * 1024)) + " MB needed, " +
              to_string(free / (1024 * 1024)) + " MB free";
        return false;
    }

    vector<string> made; // renamed into place - removed again if a later file fails
    uint64_t written = 0;
    auto undo = [&made]() {
        for (const string &path : made)
            removeFile(path);
    };
    for (const Planned &p : plan) {
        makeDirs(dirName(p.target));
        string part = p.target + ".part";
        FILE *out = openFile(part, "wb");
        if (!out) {
            why = "could not write " + part;
            undo();
            return false;
        }
        struct Sink {
            FILE *out;
            uint64_t *written;
            Progress *onPercent;
        } sink{out, &written, &onPercent};
        auto write = [](void *opaque, mz_uint64, const void *buf, size_t n) -> size_t {
            Sink *s = static_cast<Sink *>(opaque);
            size_t done = fwrite(buf, 1, n, s->out);
            *s->written += done;
            (*s->onPercent)(*s->written);
            return done;
        };
        bool ok = mz_zip_reader_extract_to_callback(&archive.zip, p.item->index, write, &sink, 0) != 0;
        ok = fclose(out) == 0 && ok;
        if (!ok) {
            removeFile(part);
            why = "could not unpack " + p.item->name + " (a damaged zip, or the stick is full)";
            undo();
            return false;
        }
        if (!renameFile(part, p.target)) {
            removeFile(part);
            why = "could not rename " + part;
            undo();
            return false;
        }
        made.push_back(p.target);
    }
    return true;
}

// the percent line, printed only when it moves
struct PercentLine {
    uint64_t total;
    int last = -1;
    void operator()(uint64_t written) {
        int percent = total == 0 ? 100 : static_cast<int>(written * 100 / total);
        if (percent != last) {
            last = percent;
            say(to_string(percent));
        }
    }
};

//******************
// --start --games <dir>
//******************
// every zip under the games tree that holds a PS1 game: one loose in Games/ gets a folder named after it,
// one already in a folder is unpacked where it is. A zip that is not a game is left alone (not ours).
void findZips(const string &dir, vector<string> &out) {
    for (const Entry &e : listDir(dir)) {
        if (e.name.empty() || e.name[0] == '.' || e.name[0] == '!') // !SaveStates, !MemCards
            continue;
        string path = dir + "/" + e.name;
        if (e.isDir)
            findZips(path, out);
        else if (endsWith(e.name, ".zip"))
            out.push_back(path);
    }
}

int startGames(const string &gamesDir) {
    vector<string> zips;
    findZips(gamesDir, zips);

    vector<string> games;
    for (const string &zip : zips) {
        Archive archive;
        if (archive.open(zip) && isPs1Game(archive))
            games.push_back(zip);
    }

    int failed = 0;
    string firstReason;
    for (size_t i = 0; i < games.size(); ++i) {
        const string &zip = games[i];
        say("#Unpacking " + fileName(zip));
        say(to_string(i + 1) + "/" + to_string(games.size()));
        string why;
        bool ok = false;
        { // the zip is closed at the end of this block: Windows will not delete an open file
            Archive archive;
            if (archive.open(zip)) {
                string parent = dirName(zip);
                string stem = fileName(zip).substr(0, fileName(zip).size() - 4);
                string dest = withoutSlash(parent) == gamesDir ? parent + "/" + stem : parent;
                ok = unpack(archive, dest, false, why, PercentLine{archive.totalSize});
            } else {
                why = "not a zip this program can read";
            }
        }
        if (ok) {
            if (!removeFile(zip))
                say("#WARN - " + fileName(zip) + " was unpacked but could not be deleted");
        } else {
            ++failed;
            if (firstReason.empty())
                firstReason = fileName(zip) + ": " + why;
            say("#WARN - " + fileName(zip) + ": " + why);
        }
    }
    if (failed > 0) {
        say("#ERROR - " + to_string(failed) + " of " + to_string(games.size()) + " zips could not be unpacked (" +
            firstReason + ")");
        return 1;
    }
    say("#DONE");
    return 0;
}

//******************
// --ismine / --start --rom <file> --system <name>
//******************
// one file inside and not an arcade system: the ROM itself, next to where the zip was
bool romIsMine(const string &file, const string &system) {
    if (!endsWith(file, ".zip") || keepsZips(system))
        return false;
    Archive archive;
    return archive.open(file) && archive.items.size() == 1;
}

int startRom(const string &file, const string &system) {
    if (!romIsMine(file, system)) {
        say("#DONE"); // not ours: nothing to do is a success
        return 0;
    }
    say("#Unpacking " + fileName(file));
    string why;
    bool ok;
    {
        Archive archive;
        ok = archive.open(file) && unpack(archive, dirName(file), true, why, PercentLine{archive.totalSize});
    }
    if (!ok) {
        say("#ERROR - " + fileName(file) + ": " + why);
        return 1;
    }
    if (!removeFile(file))
        say("#WARN - " + fileName(file) + " was unpacked but could not be deleted");
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
