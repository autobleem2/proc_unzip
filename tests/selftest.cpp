//
// unzip_selftest: builds zips with miniz, runs the real unzip program on them and checks what the protocol and
// the processor rules promise - the output lines, the exit codes, the files, and that a second run does
// nothing. Run by ctest from the build directory (a scratch tree is made there).
//
#include "miniz.h"

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

bool exists(const string &path) {
    ifstream f(path);
    return f.good();
}

void writeFile(const string &path, const string &text) {
    ofstream(path, ios::binary) << text;
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
    check(v.code == 0 && v.has("#Unzip V1.0.0 - Unpacks zipped PS1 games and ROMs"), "--version");

    Run g = run("--start --games " + games);
    check(g.lines.size() > 0 && g.lines[0] == "#Starting - Unzip V1.0.0", "the first line is #Starting");
    check(g.has("#Unpacking Crash (USA).zip") && g.has("100"), "a stage and a percent per zip");
    check(g.code == 1 && g.starts("#ERROR - 1 of 3 zips could not be unpacked"), "the collision fails the run");
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

    if (failures == 0)
        cout << "unzip_selftest: all checks passed" << endl;
    return failures == 0 ? 0 : 1;
}
