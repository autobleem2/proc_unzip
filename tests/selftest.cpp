//
// unzip_selftest: builds zips with miniz, copies the 7z and RAR archives in tests/data, runs the real unzip
// program on them and checks what the protocol and the processor rules promise - the output lines, the exit
// codes, the files, and that a second run does nothing. Run by ctest from the build directory (a scratch tree
// is made there).
//
// tests/data was made with 7-Zip 16.02 and RAR 7.01 (RAR5) / 6.23 (RAR4, which RAR 7 no longer writes); every
// .bin in it holds binData(size), the .cue says FILE "<name>.bin" BINARY:
//   crash.7z        Crash (USA)/Crash (USA).{cue,bin}             LZMA2, solid, 200000
//   spyro.7z        Spyro.{cue,bin}                               PPMd, 200000
//   pokemon.7z      Pokémon (Europe)/Pokémon (Europe).{cue,bin}   a UTF-16 name in a 7z, 200000
//   tekken.rar      Tekken 3 (USA)/Tekken 3 (USA).{cue,bin}       RAR5, 200000
//   ridge.rar       Ridge Racer.{cue,bin}                         RAR4, 200000
//   Gran Turismo.part1-3.rar   Gran Turismo.{cue,bin}             RAR5 volumes, stored, 30000
//   Wipeout.rar, .r00, .r01    Wipeout.{cue,bin}                  RAR4 volumes, old names, 600000
//   sonic.7z        Sonic (World).md    mario.rar  Mario (World).nes     4096 each
//   secret.7z       Sonic (World).md, with a password             docs.7z   readme.txt
//
#include "miniz.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define POPEN _popen
#define PCLOSE _pclose
#else
#include <sys/stat.h>
#include <sys/wait.h>
#define MKDIR(p) mkdir(p, 0777)
#define POPEN popen
#define PCLOSE pclose
#endif

using namespace std;

namespace {

int failures = 0;

void check(bool ok, const string &what) {
    if (!ok) {
        ++failures;
        cerr << "FAILED: " << what << endl;
    }
}

// paths are UTF-8, as the program's are ("Pokémon")
FILE *openFile(const string &path, const char *mode) {
#ifdef _WIN32
    auto wide = [](const string &s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    };
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

bool exists(const string &path) {
    FILE *f = openFile(path, "rb");
    if (f)
        fclose(f);
    return f != nullptr;
}

void writeFile(const string &path, const string &text) {
    ofstream(path, ios::binary) << text;
}

// a whole file; "<missing>" when it cannot be read
string readFile(const string &path) {
    FILE *f = openFile(path, "rb");
    if (!f)
        return "<missing>";
    string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    fclose(f);
    return text;
}

void copyData(const string &name, const string &toDir) {
    writeFile(toDir + "/" + name, readFile(string(TEST_DATA) + "/" + name));
}

// what every .bin in tests/data holds (the fixture script made them the same way)
string binData(size_t n) {
    string s(n, '\0');
    for (size_t i = 0; i < n; ++i)
        s[i] = static_cast<char>((static_cast<uint64_t>(i) * i / 97) & 0xFF);
    return s;
}

// the game a fixture unpacked to `dir`: its cue and bin, each what it should be
bool unpackedGame(const string &dir, const string &name, size_t size) {
    return readFile(dir + "/" + name + ".cue") == "FILE \"" + name + ".bin\" BINARY\n" &&
           readFile(dir + "/" + name + ".bin") == binData(size);
}

// a zip of {name, contents}
void makeZip(const string &path, const vector<pair<string, string>> &files) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    mz_zip_writer_init_file(&zip, path.c_str(), 0);
    for (const auto &f : files)
        mz_zip_writer_add_mem(&zip, f.first.c_str(), f.second.data(), f.second.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
}

struct Run {
    int code;
    vector<string> lines;
    bool has(const string &line) const {
        for (const string &l : lines) {
            if (l == line)
                return true;
        }
        return false;
    }
    bool starts(const string &prefix) const {
        for (const string &l : lines) {
            if (l.compare(0, prefix.size(), prefix) == 0)
                return true;
        }
        return false;
    }
};

Run run(const string &args) {
    string cmd = string("\"") + UNZIP_EXE + "\" " + args;
#ifdef _WIN32
    cmd = "\"" + cmd + "\""; // cmd.exe strips one pair of quotes around the whole line
#endif
    Run r{-1, {}};
    FILE *p = POPEN(cmd.c_str(), "r");
    if (!p)
        return r;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) {
        string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        r.lines.push_back(line);
    }
    int status = PCLOSE(p);
#ifdef _WIN32
    r.code = status;
#else
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return r;
}

} // namespace

int main() {
    const string root = "selftest_tree";
#ifdef _WIN32
    system(("if exist " + root + " rmdir /s /q " + root).c_str());
#else
    system(("rm -rf " + root).c_str());
#endif
    MKDIR(root.c_str());
    const string games = root + "/Games";
    MKDIR(games.c_str());
    MKDIR((games + "/Spyro").c_str());
    MKDIR((games + "/Taken").c_str());
    const string roms = root + "/roms";
    MKDIR(roms.c_str());
    MKDIR((roms + "/Sega - Mega Drive - Genesis").c_str());
    MKDIR((roms + "/MAME").c_str());

    // a loose game zip with a top folder, a zip inside a game folder, a zip that is no game, a collision
    makeZip(games + "/Crash (USA).zip",
            {{"Crash (USA)/Crash (USA).cue", "FILE \"Crash (USA).bin\" BINARY\n"}, {"Crash (USA)/Crash (USA).bin", string(200000, 'x')}});
    makeZip(games + "/Spyro/Spyro.zip", {{"Spyro.bin", "bin"}, {"Spyro.cue", "cue"}});
    makeZip(games + "/docs.zip", {{"readme.txt", "hello"}});
    writeFile(games + "/Taken/Taken.bin", "already here");
    makeZip(games + "/Taken/Taken.zip", {{"Taken.bin", "new"}, {"Taken.cue", "cue"}});

    Run v = run("--version");
    check(v.code == 0 && v.has("#Unzip V1.1.0 - Unpacks PS1 games and ROMs from .zip, .7z and .rar"), "--version");

    Run g = run("--start --games " + games);
    check(g.lines.size() > 0 && g.lines[0] == "#Starting - Unzip V1.1.0", "the first line is #Starting");
    check(g.has("#Unpacking Crash (USA).zip") && g.has("100"), "a stage and a percent per zip");
    check(g.code == 1 && g.starts("#ERROR - 1 of 3 archives could not be unpacked"), "the collision fails the run");
    check(g.starts("#WARN - Taken.zip: Taken.bin is already there"), "the collision's warning");
    check(exists(games + "/Crash (USA)/Crash (USA).cue") && exists(games + "/Crash (USA)/Crash (USA).bin"),
          "a loose zip gets a folder, the top folder dropped");
    check(!exists(games + "/Crash (USA).zip"), "an unpacked zip is deleted");
    check(exists(games + "/Spyro/Spyro.bin") && !exists(games + "/Spyro/Spyro.zip"), "a zip in a folder unpacks there");
    check(exists(games + "/docs.zip"), "a zip that is not a game is left alone");
    check(exists(games + "/Taken/Taken.zip") && !exists(games + "/Taken/Taken.cue") &&
              !exists(games + "/Taken/Taken.bin.part"),
          "a failed zip is kept and leaves nothing behind");
    {
        ifstream taken(games + "/Taken/Taken.bin");
        string kept((istreambuf_iterator<char>(taken)), istreambuf_iterator<char>());
        check(kept == "already here", "an existing file is never overwritten");
    } // closed: Windows will not delete an open file

    // idempotent: once the collision is gone, a second run does the last zip; a third does nothing
    remove((games + "/Taken/Taken.bin").c_str());
    Run g2 = run("--start --games " + games);
    check(g2.code == 0 && g2.has("#DONE"), "the second run finishes the rest");
    Run g3 = run("--start --games " + games);
    check(g3.code == 0 && g3.lines.size() == 2 && g3.lines[1] == "#DONE", "nothing to do: #Starting and #DONE only");

    // a run stopped in the middle left the first file in place and the second as .part: a restart
    // keeps the first, rewrites the second, and deletes the zip
    makeZip(games + "/Resumed.zip", {{"Resumed.cue", "cue"}, {"Resumed.bin", "0123456789"}});
    MKDIR((games + "/Resumed").c_str());
    writeFile(games + "/Resumed/Resumed.cue", "cue");
    writeFile(games + "/Resumed/Resumed.bin.part", "01234");
    Run resumed = run("--start --games " + games);
    check(resumed.code == 0 && resumed.has("#DONE"), "a stopped run's zip finishes on the restart");
    {
        ifstream bin(games + "/Resumed/Resumed.bin");
        string text((istreambuf_iterator<char>(bin)), istreambuf_iterator<char>());
        check(text == "0123456789", "the half-written file is written again, whole");
    }
    check(!exists(games + "/Resumed/Resumed.bin.part") && !exists(games + "/Resumed.zip"),
          "no .part left, the zip deleted");

    // ROMs
    const string sonic = roms + "/Sega - Mega Drive - Genesis/Sonic.zip";
    makeZip(sonic, {{"Sonic (World).md", "rom"}});
    const string pacman = roms + "/MAME/pacman.zip";
    makeZip(pacman, {{"pacman.6e", "a"}});
    check(run("--ismine --rom \"" + sonic + "\" --system \"Sega - Mega Drive - Genesis\"").code == 0, "one ROM: mine");
    check(run("--ismine --rom \"" + pacman + "\" --system MAME").code == 1, "an arcade set: not mine");
    Run r = run("--start --rom \"" + sonic + "\" --system \"Sega - Mega Drive - Genesis\"");
    check(r.code == 0 && r.has("#DONE"), "a ROM unpacks");
    check(exists(roms + "/Sega - Mega Drive - Genesis/Sonic (World).md") && !exists(sonic), "next to where the zip was");
    Run a = run("--start --rom \"" + pacman + "\" --system MAME");
    check(a.code == 0 && exists(pacman), "an arcade set is left zipped");

    // 7z and RAR games: every format and method in tests/data, the volumes of a set, a UTF-16 name
    const string more = root + "/Games2";
    MKDIR(more.c_str());
    MKDIR((more + "/Spyro").c_str());
    for (const char *name : {"crash.7z", "pokemon.7z", "tekken.rar", "ridge.rar", "Gran Turismo.part1.rar",
                             "Gran Turismo.part2.rar", "Gran Turismo.part3.rar", "Wipeout.rar", "Wipeout.r00",
                             "Wipeout.r01", "docs.7z"})
        copyData(name, more);
    copyData("spyro.7z", more + "/Spyro");
    Run m = run("--start --games \"" + more + "\"");
    check(m.code == 0 && m.has("#DONE"), "7z and RAR games unpack");
    check(m.has("#Unpacking Gran Turismo.part1.rar") && !m.has("#Unpacking Gran Turismo.part2.rar"),
          "a set of volumes is one archive, named by its first");
    check(m.has("7/7"), "seven games: the one that is no game is not counted");
    check(unpackedGame(more + "/crash", "Crash (USA)", 200000), "7z, LZMA2: the top folder dropped");
    check(unpackedGame(more + "/Spyro", "Spyro", 200000), "7z, PPMd: in its folder");
    check(unpackedGame(more + "/pokemon", "Pok\xC3\xA9mon (Europe)", 200000), "a 7z's UTF-16 name, in UTF-8");
    check(unpackedGame(more + "/tekken", "Tekken 3 (USA)", 200000), "RAR5");
    check(unpackedGame(more + "/ridge", "Ridge Racer", 200000), "RAR4");
    check(unpackedGame(more + "/Gran Turismo", "Gran Turismo", 30000), "RAR5 volumes: .part1.rar, ...");
    check(unpackedGame(more + "/Wipeout", "Wipeout", 600000), "RAR4 volumes: .rar, .r00, ...");
    check(!exists(more + "/crash.7z") && !exists(more + "/Spyro/spyro.7z") && !exists(more + "/tekken.rar") &&
              !exists(more + "/Gran Turismo.part1.rar") && !exists(more + "/Gran Turismo.part3.rar") &&
              !exists(more + "/Wipeout.rar") && !exists(more + "/Wipeout.r01"),
          "every archive deleted, every volume of a set too");
    check(exists(more + "/docs.7z"), "a 7z that is not a game is left alone");

    // a 7z or RAR tells no CRC: a file already there is compared byte by byte as the archive is unpacked -
    // the same bytes are a stopped run's work (kept, the archive deleted), others are a clash (nothing touched)
    copyData("tekken.rar", more);
    string ridgeBin = more + "/ridge/Ridge Racer.bin";
    string changed = binData(200000);
    changed[150000] ^= 0x55;
    writeFile(ridgeBin, changed);
    copyData("ridge.rar", more);
    Run again = run("--start --games \"" + more + "\"");
    check(again.code == 1 && again.starts("#WARN - ridge.rar: Ridge Racer.bin is already there"),
          "a file of the same size but other bytes is a clash");
    check(!exists(more + "/tekken.rar") && unpackedGame(more + "/tekken", "Tekken 3 (USA)", 200000),
          "the same bytes: kept, and the archive deleted");
    check(exists(more + "/ridge.rar") && readFile(ridgeBin) == changed && !exists(ridgeBin + ".part"),
          "a clash keeps the archive and leaves the file as it was");

    // 7z and RAR ROMs
    const string md = roms + "/Sega - Game Gear"; // the zip above left its own Sonic (World).md in Mega Drive
    MKDIR(md.c_str());
    const string nes = roms + "/Nintendo - Nintendo Entertainment System";
    MKDIR(nes.c_str());
    copyData("sonic.7z", md);
    copyData("mario.rar", nes);
    copyData("secret.7z", nes);
    copyData("sonic.7z", roms + "/MAME");
    check(run("--ismine --rom \"" + md + "/sonic.7z\" --system \"Sega - Game Gear\"").code == 0,
          "one ROM in a 7z: mine");
    check(run("--ismine --rom \"" + roms + "/MAME/sonic.7z\" --system MAME").code == 1, "a 7z arcade set: not mine");
    check(run("--ismine --rom \"" + nes + "/secret.7z\" --system NES").code == 1, "a password: not mine");
    Run s7 = run("--start --rom \"" + md + "/sonic.7z\" --system \"Sega - Game Gear\"");
    Run rr = run("--start --rom \"" + nes + "/mario.rar\" --system \"Nintendo - Nintendo Entertainment System\"");
    check(s7.code == 0 && rr.code == 0, "7z and RAR ROMs unpack");
    check(readFile(md + "/Sonic (World).md") == binData(4096) && !exists(md + "/sonic.7z"), "the 7z's ROM");
    check(readFile(nes + "/Mario (World).nes") == binData(4096) && !exists(nes + "/mario.rar"), "the RAR's ROM");
    check(exists(nes + "/secret.7z") && exists(roms + "/MAME/sonic.7z"), "what is not mine stays packed");

    if (failures == 0)
        cout << "unzip_selftest: all checks passed" << endl;
    return failures == 0 ? 0 : 1;
}
