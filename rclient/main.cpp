#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <QtCore>
#include <getopt.h>
#include <RTags.h>
#include <Location.h>
#include <AtomicString.h>
#include "Database.h"
#include "Mmap.h"

enum Flag {
    PathsRelativeToRoot = 0x1,
    NoContext = 0x2,
    PrintDBPath = 0x4,
    SeparateLocationsBySpace = 0x8
};

// This sorts in reverse order. Location with higher line, column comes first
static inline bool compareLoc(const Location &l, const Location &r)
{
    if (l.file < r.file)
        return true;
    if (r.file > l.file)
        return false;
    if (l.line > r.line)
        return true;
    if (l.line < r.line)
        return false;
    if (l.column <= r.column)
        return false;
    return false;
}

using namespace RTags;
static inline int readLine(FILE *f, char *buf, int max)
{
    assert(!buf == (max == -1));
    if (max == -1)
        max = INT_MAX;
    for (int i=0; i<max; ++i) {
        const int ch = fgetc(f);
        if (ch == '\n' || ch == EOF)
            return i;
        if (buf)
            *buf++ = *reinterpret_cast<const char*>(&ch);
    }
    return -1;
}
QHash<const Database*, QByteArray> sUsedDBs;
static inline bool printLocation(const Location &loc, const Database *db,
                                 unsigned flags, QSet<Location> *filter = 0)
{
    if (filter) {
        if (filter->contains(loc))
            return false;
        filter->insert(loc);
    }
    QByteArray out = db->locationToString(loc);
    if (out.isEmpty())
        return false;
    if (flags & PathsRelativeToRoot) {
        const Path p = db->path().parentDir();
        if (out.startsWith(p)) {
            out.remove(0, p.endsWith('/') ? p.size() : p.size() + 1);
        }
    }

    const char sep = (flags & SeparateLocationsBySpace ? ' ' : '\n');
    if (flags & NoContext) {
        printf("%s%c", out.constData(), sep);
        return false;
    }

    const Path p = db->path(loc);
    if (p.isEmpty())
        return false;

    QByteArray &ref = sUsedDBs[db];
    if (ref.isEmpty())
        ref = db->path();
    
    printf("%s", out.constData());

    FILE *f = fopen(p.constData(), "r");
    if (f) {
        Q_ASSERT(loc.line > 0);
        for (unsigned i=0; i<loc.line - 1; ++i)
            readLine(f, 0, -1);
        char buf[1024] = { 0 };
        readLine(f, buf, 1024);
        printf("\t%s%c", buf, sep);
        fclose(f);
    } else {
        printf("%c", sep);
    }
    return true;
}

static inline void usage(const char* argv0, FILE *f)
{
    fprintf(f,
            "%s [options]...\n"
            "  --help|-h                     Display this help\n"
            "  --db-file|-d [arg]            Find database using this path\n"
            "  --print-db-path|-p            Print out the used database path(s)\n"
            "  --detect-db|-D                Find .rtags.db based on path\n"
            "                                (default when no -d options are specified)\n"
            "  --db-type|-t [arg]            Type of db (leveldb or filedb)\n"
            "  --paths-relative-to-root|-n   Print out files matching arg\n"
            "  --no-context|-N               Don't print context from files when printing locations\n"
            "  --separate-paths-by-space     Separate multiple locations by space instead of newline\n"
            "\n"
            "  Modes\n"
            "  --follow-symbol|-f [arg]      Follow this symbol (e.g. /tmp/main.cpp:32:1)\n"
            "  --find-references|-r [arg]    Print references of symbol at arg\n"
            "  --list-symbols|-l [arg]       Print out symbols names matching arg\n"
            "  --files|-P [arg]              Print out files matching arg\n"
            "  --all-references|-a [arg]     Print all references/declarations/definitions that matches arg\n"
            "  --rename-symbol|-R [loc] [to] Rename symbol. E.g. rc -R main.cpp:10:3 foobar\n"
            "  --find-symbols|-s [arg]       Print out symbols matching arg\n",
            argv0);
}

int main(int argc, char** argv)
{
    {
        QFile log("/tmp/rc.log");
        log.open(QIODevice::Append);
        char buf[512];
        const bool cwd = getcwd(buf, 512);
        if (cwd) {
            log.write("( cd ");
            log.write(buf);
            log.write(" && ");
        }
        
        for (int i=0; i<argc; ++i) {
            log.write(argv[i]);
            log.putChar(' ');
        }
        if (cwd)
            log.write(" )");
        log.putChar('\n');
    }

    struct option longOptions[] = {
        { "help", no_argument, 0, 'h' },
        { "follow-symbol", required_argument, 0, 'f' },
        { "db", required_argument, 0, 'd' },
        { "print-db-path", no_argument, 0, 'p' },
        { "find-references", required_argument, 0, 'r' },
        { "find-symbols", required_argument, 0, 's' },
        { "find-db", no_argument, 0, 'F' },
        { "list-symbols", optional_argument, 0, 'l' },
        { "files", optional_argument, 0, 'P' },
        { "paths-relative-to-root", no_argument, 0, 'n' },
        { "db-type", required_argument, 0, 't' },
        { "all-references", required_argument, 0, 'a' },
        { "rename-symbol", required_argument, 0, 'R' },
        { "no-context", no_argument, 0, 'N' },
        { "separate-paths-by-space", no_argument, 0, 'S' },
        { 0, 0, 0, 0 },
    };
    const char *shortOptions = "hf:d:r:l::Dps:P::nt:a:R:NS";

    Mmap::init();

    QList<QByteArray> dbPaths;

    enum Mode {
        None,
        FollowSymbol,
        References,
        FindSymbols,
        ListSymbols,
        Files,
        AllReferences,
        RenameSymbol
        // RecursiveReferences,
    } mode = None;
    unsigned flags = 0;
    int idx, longIndex;
    QByteArray arg, renameTo;
    opterr = 0;
    while ((idx = getopt_long(argc, argv, shortOptions, longOptions, &longIndex)) != -1) {
        switch (idx) {
        case '?':
            usage(argv[0], stderr);
            fprintf(stderr, "rc: invalid option \"%s\"\n", argv[optind]);
            return 1;
        case 'N':
            flags |= NoContext;
            break;
        case 'S':
            flags |= SeparateLocationsBySpace;
            break;
        case 'R':
            if (mode != None) {
                fprintf(stderr, "Mode is already set\n");
                return 1;
            }

            if (optind >= argc) {
                fprintf(stderr, "Invalid args for -R\n");
                return 1;
            }
            mode = RenameSymbol;
            renameTo = argv[optind];
            ++optind;
            break;
        case 'a':
            if (mode != None) {
                fprintf(stderr, "Mode is already set\n");
                return 1;
            }
            mode = AllReferences;
            arg = optarg;
            break;
        case 'n':
            flags |= PathsRelativeToRoot;
            break;
        case 'p':
            flags |= PrintDBPath;
            break;
        case 't':
            setenv("RTAGS_DB_TYPE", optarg, 1);
            break;
        case 'h':
            usage(argv[0], stdout);
            return 0;
        case 'f':
            if (mode != None) {
                fprintf(stderr, "Mode is already set\n");
                return 1;
            }
            arg = optarg;
            mode = FollowSymbol;
            break;
        case 'P':
            if (mode != None) {
                fprintf(stderr, "Mode is already set\n");
                return 1;
            }
            arg = optarg;
            mode = Files;
            break;
        case 'r':
            arg = optarg;
            if (mode != None) {
                fprintf(stderr, "Mode is already set\n");
                return 1;
            }
            mode = References;
            break;
        case 'D': {
            const QByteArray db = findRtagsDb();
            if (!db.isEmpty()) {
                dbPaths.append(db);
            }
            break; }
        case 'd':
            if (optarg && strlen(optarg)) {
                const QByteArray db = findRtagsDb(optarg);
                if (!db.isEmpty())
                    dbPaths.append(db);
            }
            break;
        case 'l':
            if (mode != None) {
                fprintf(stderr, "Mode is already set\n");
                return 1;
            }
            mode = ListSymbols;
            arg = optarg;
            break;
        case 's':
            if (mode != None) {
                fprintf(stderr, "Mode is already set\n");
                return 1;
            }
            mode = FindSymbols;
            arg = optarg;
            break;
        }
    }
    if (dbPaths.isEmpty()) {
        QByteArray db = findRtagsDb();
        if (db.isEmpty() && !arg.isEmpty())
            db = findRtagsDb(arg);
        if (!db.isEmpty())
            dbPaths.append(db);
    }

    if (dbPaths.isEmpty()) {
        fprintf(stderr, "No databases specified\n");
        return 1;
    }
    // if ((flags & PathsRelativeToRoot) && mode != Files) {
    //     fprintf(stderr, "-n only makes sense with -P\n");
    //     return 1;
    // }
    bool done = false;
    foreach(const QByteArray &dbPath, dbPaths) {
        if (dbPath.isEmpty())
            continue;
        Database* db = Database::create(dbPath, Database::ReadOnly);
        if (!db->isOpened()) {
            delete db;
            continue;
        }

        switch (mode) {
        case None:
            usage(argv[0], stderr);
            fprintf(stderr, "No mode selected\n");
            return 1;
        case RenameSymbol:
        case AllReferences: {
            const Location loc = db->createLocation(arg);
            if (!loc.file) {
                fprintf(stderr, "Invalid arg %s", arg.constData());
                break;
            }
            QList<Location> all = db->allLocations(loc).toList();
            qSort(all.begin(), all.end(), compareLoc);
            if (mode == AllReferences) {
                foreach(const Location &l, all) {
                    printed |= printLocation(l, db, flags);
                }
            } else {
                foreach(const Location &l, all) {
                    const Path p = db->file(l.file);
                    FILE *f = fopen(p.constData(), "r+");
                    if (!f) {
                        fprintf(stderr, "Can't open %s for writing\n", p.constData());
                        continue;
                    }
                    for (unsigned i=0; i<l.line - 1; ++i) {
                        if (readLine(f, 0, 0) == -1) {
                            fprintf(stderr, "Read error for %s. Can't read line %d\n",
                                    p.constData(), i + 1);
                        }
                    }
                    fseek(f, l.column - 1, SEEK_CUR);
                    long old = ftell(f);
                    fwrite(renameTo.constData(), sizeof(char), renameTo.size(), f);
                    printf("fixed shit %ld %ld\n", old, ftell(f));
                }
            }
            break; }
        case FollowSymbol: {
            Location loc = db->createLocation(arg);
            // printf("%s => %d:%d:%d\n", arg.constData(), loc.file, loc.line, loc.column);
            if (loc.file) {
                done = printLocation(db->followLocation(loc), db, flags);
                // we're not going to find more than one followLocation
            } else {
                QSet<Location> filter;
                foreach(const Location &l, db->findSymbol(arg)) {
                    printLocation(db->followLocation(l), db, flags, &filter);
                }
            }
            break; }
        case References: {
            const Location loc = db->createLocation(arg);
            if (loc.file) {
                foreach(const Location &l, db->findReferences(loc)) {
                    printLocation(l, db, flags);
                }
            } else {
                QSet<Location> filter;
                foreach(const Location &l, db->findSymbol(arg)) {
                    foreach(const Location &r, db->findReferences(l)) {
                        printLocation(r, db, flags, &filter);
                    }
                }
            }
            break; }
        case FindSymbols:
            foreach(const Location &loc, db->findSymbol(arg)) {
                printLocation(loc, db, flags);
            }
            break;
        case ListSymbols: {
            const QList<QByteArray> symbolNames = db->symbolNames(arg);
            if (!symbolNames.isEmpty())
                sUsedDBs[db] = db->path();
            foreach(const QByteArray &symbol, symbolNames) {
                printf("%s\n", symbol.constData());
            }
            break; }
        case Files: {
            QSet<Path> paths = db->read<QSet<Path> >("files");
            if (!paths.isEmpty())
                sUsedDBs[db] = db->path();
            const bool empty = arg.isEmpty();
            const char *root = "./";
            Path srcDir;
            if (!(flags & PathsRelativeToRoot)) {
                srcDir = db->read<Path>("sourceDir");
                root = srcDir.constData();
            }
            foreach(const Path &path, paths) {
                if (empty || path.contains(arg)) {
                    printf("%s%s\n", root, path.constData());
                }
            }
            break; }
        }
        if (done)
            break;
    }
    if (flags & PrintDBPath) {
        foreach(const QByteArray &db, sUsedDBs) {
            printf("_[DB: %s]_\n", db.constData());
        }
    }

    return 0;
}
