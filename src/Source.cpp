/* This file is part of RTags (http://rtags.net).

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#include "Source.h"

#include "Location.h"
#include "rct/EventLoop.h"
#include "RTags.h"
#include "Server.h"

extern const Server::Options *serverOptions();

void Source::clear()
{
    fileId = compilerId = buildRootId = 0;
    includePathHash = 0;
    language = NoLanguage;
    parsed = 0;

    defines.clear();
    includePaths.clear();
    arguments.clear();
}

Path Source::sourceFile() const
{
    return Location::path(fileId);
}

Path Source::buildRoot() const
{
    return Location::path(buildRootId);
}

Path Source::compiler() const
{
    return Location::path(compilerId);
}

String Source::toString() const
{
    String ret = String::join(toCommandLine(IncludeCompiler|IncludeSourceFile|IncludeIncludepaths|QuoteDefines|IncludeDefines), ' ');
    if (buildRootId)
        ret << " Build: " << buildRoot();
    if (parsed)
        ret << " Parsed: " << String::formatTime(parsed / 1000, String::DateTime);
    if (flags & Active)
        ret << " Active";
    return ret;
}

static inline Source::Language guessLanguageFromSourceFile(const Path &sourceFile)
{
    // ### We should support some more of of these really
    // .Case("cl", IK_OpenCL)
    // .Case("cuda", IK_CUDA)
    // .Case("c++", IK_CXX)
    // .Case("objective-c", IK_ObjC)
    // .Case("objective-c++", IK_ObjCXX)
    // .Case("cpp-output", IK_PreprocessedC)
    // .Case("assembler-with-cpp", IK_Asm)
    // .Case("c++-cpp-output", IK_PreprocessedCXX)
    // .Case("objective-c-cpp-output", IK_PreprocessedObjC)
    // .Case("objc-cpp-output", IK_PreprocessedObjC)
    // .Case("objective-c++-cpp-output", IK_PreprocessedObjCXX)
    // .Case("objc++-cpp-output", IK_PreprocessedObjCXX)
    // .Case("c-header", IK_C)
    // .Case("cl-header", IK_OpenCL)
    // .Case("objective-c-header", IK_ObjC)
    // .Case("c++-header", IK_CXX)
    // .Case("objective-c++-header", IK_ObjCXX)

    const char *suffix = sourceFile.extension();
    if (suffix) {
        if (!strcasecmp(suffix, "cpp")) {
            return Source::CPlusPlus;
        } else if (!strcasecmp(suffix, "cc")) {
            return Source::CPlusPlus;
        } else if (!strcmp(suffix, "C")) {
            return Source::CPlusPlus;
        } else if (!strcmp(suffix, "cp")) {
            return Source::CPlusPlus;
        } else if (!strcmp(suffix, "cxx")) {
            return Source::CPlusPlus;
        } else if (!strcmp(suffix, "c++")) {
            return Source::CPlusPlus;
        } else if (!strcmp(suffix, "c")) {
            return Source::C;
        } else if (!strcmp(suffix, "M")) {
            return Source::ObjectiveCPlusPlus;
        } else if (!strcmp(suffix, "mm")) {
            return Source::ObjectiveCPlusPlus;
        } else if (!strcmp(suffix, "m")) {
            return Source::ObjectiveC;
        }
    }
    return Source::NoLanguage;
}

static Path findFileInPath(const Path &unresolved, const Path &cwd, const List<Path> &pathEnvironment)
{
    // error() << "Coming in with" << front;
    Path resolve;
    Path file;
    if (unresolved.isAbsolute()) {
        resolve = unresolved;
    } else if (unresolved.contains('/')) {
        assert(cwd.endsWith('/'));
        resolve = cwd + unresolved;
    } else {
        file = unresolved;
    }

    if (!resolve.isEmpty()) {
        const Path resolved = resolve.resolved();
        if (!strcmp(resolved.fileName(), "gcc-rtags-wrapper.sh")) {
            file = unresolved.fileName();
        } else {
            return resolve;
        }
        file = unresolved.fileName();
    }

    for (const Path &path : pathEnvironment) {
        bool ok;
        const Path p = Path::resolved(file, Path::RealPath, path, &ok);
        if (ok) {
            if (strcmp(p.fileName(), "gcc-rtags-wrapper.sh") && !access(p.nullTerminated(), R_OK | X_OK)) {
                debug() << "Found compiler" << p << "for" << unresolved;
                return Path::resolved(file, Path::MakeAbsolute, path);
            }
        }
    }
    return unresolved;
}

static inline String trim(const char *start, int size)
{
    while (size && isspace(*start)) {
        ++start;
        --size;
    }
    while (size && isspace(start[size - 1])) {
        --size;
    }
    return String(start, size);
}

static inline size_t hashIncludePaths(const List<Source::Include> &includes, const Path &buildRoot)
{
    size_t hash = 0;
    std::hash<Path> hasher;
    for (const auto &inc : includes) {
        size_t h;
        if (!buildRoot.isEmpty() && inc.path.startsWith(buildRoot)) {
            h = hasher(inc.path.mid(buildRoot.size()));
        } else {
            h = hasher(inc.path);
        }
        h += inc.type;
        hash ^= h + 0x9e3779b9 + (h << 6) + (h >> 2);
        // Bit twiddling found here:
        // http://stackoverflow.com/questions/15741615/c-suggestions-about-a-hash-function-for-a-sequence-of-strings-where-the-order
        // apparently from boost.
    }
    return hash;
}

static inline void addIncludeArg(List<Source::Include> &includePaths,
                                 Source::Include::Type type,
                                 int argLen,
                                 const List<String> &args,
                                 int &idx,
                                 const Path &cwd)
{
    const String &arg = args.at(idx);
    Path path;
    if (arg.size() == argLen) {
        path = Path::resolved(args.value(++idx), Path::MakeAbsolute, cwd);
    } else {
        path = Path::resolved(arg.mid(argLen), Path::MakeAbsolute, cwd);
    }
    includePaths.append(Source::Include(type, path));
}

static const char* valueArgs[] = {
    "-I",
    "-o",
    "-x",
    "-target",
    "--param",
    "-imacros",
    "-iprefix",
    "-iwithprefix",
    "-iwithprefixbefore",
    "-imultilib",
    "-isysroot",
    "-ivfsoverlay",
    "-Xpreprocessor",
    "-Xassembler",
    "-Xlinker",
    "-Xclang",
    "-Xanalyzer",
    "-T",
    "-V",
    "-b",
    "-G",
    "-arch",
    "-MF",
    "-MT",
    "-MQ",
    "-gcc-toolchain",
    "-include",
    0
};

static const char *blacklist[] = {
    "-M",
    "-MM",
    "-MG",
    "-MP",
    "-MD",
    "-MMD",
    "-MF",
    "-MT",
    "-MQ",
    "-gcc-toolchain",
    "-fno-var-tracking",
    "-fvar-tracking",
    "-fno-var-tracking-assignments",
    "-fvar-tracking-assignments",
    "-fvar-tracking-assignments-toggle",
    "-Og",
    0
};

static inline bool hasValue(const String &arg)
{
    for (int i = 0; valueArgs[i]; ++i) {
        if (arg == valueArgs[i])
            return true;
    }

    if (const auto *opts = serverOptions()) {
        for (const String &blockedArg : opts->blockedArguments) {
            if (blockedArg.endsWith('=') && blockedArg.startsWith(arg)) {
                return true;
            }
        }
    }

    return false;
}

static inline bool isBlacklisted(const String& arg)
{
    for (int i = 0; blacklist[i]; ++i) {
        if (arg == blacklist[i])
            return true;
    }
    return false;
}

static inline String unquote(const String& arg)
{
    if (arg.size() >= 4 && arg.startsWith("\\\"") && arg.endsWith("\\\"")) {
        return arg.mid(1, arg.size() - 3) + '\"';
    } else if (arg.size() >= 2 && arg.startsWith('"') && arg.endsWith('"')) {
        return arg.mid(1, arg.size() - 2);
    }
    return arg;
}

static Path resolveCompiler(const Path &unresolved, const Path &cwd, const List<Path> &pathEnvironment)
{
    assert(EventLoop::isMainThread()); // not threadsafe
    static Hash<Path, Path> resolvedFromPath;
    Path &compiler = resolvedFromPath[unresolved];
    if (compiler.isEmpty())
        compiler = findFileInPath(unresolved, cwd, pathEnvironment);

    if (!compiler.isFile()) {
        compiler.clear();
    } else if (compiler.contains("..")) {
        compiler.canonicalize();
    }
    return compiler;
}


static inline bool isCompiler(const Path &fullPath)
{
    assert(EventLoop::isMainThread());

    if (const auto *opts = serverOptions()) {
        for (const auto &rx : opts->extraCompilers) {
            if (Rct::contains(fullPath, rx))
                return true;
        }
    }

    String compiler = fullPath.fileName();
    if (compiler.endsWith(".exe"))
        return true;

    String c;
    int dash = compiler.lastIndexOf('-');
    if (dash >= 0) {
        c = String(compiler.constData() + dash + 1, compiler.size() - dash - 1);
    } else {
        c = String(compiler.constData(), compiler.size());
    }

    if (c.size() != compiler.size()) {
        bool isVersion = true;
        for (int i=0; i<c.size(); ++i) {
            if ((c.at(i) < '0' || c.at(i) > '9') && c.at(i) != '.') {
#ifdef OS_CYGWIN
                // eat 'exe' if it exists
                if (c.mid(i) == "exe")
                    goto cont;
#endif
                isVersion = false;
                break;
            }
        }
#ifdef OS_CYGWIN
  cont:
#endif
        if (isVersion) {
            dash = compiler.lastIndexOf('-', dash - 1);
            if (dash >= 0) {
                c = compiler.mid(dash + 1, compiler.size() - c.size() - 2 - dash);
            } else {
                c = compiler.left(dash);
            }
        }
    }


    return (c.startsWith("g++")
            || c.startsWith("c++")
            || c.startsWith("clang")
            || c.startsWith("gcc")
            || c.startsWith("cc"));
}

struct Input {
    Path realPath, absolute;
    Source::Language language;
};

List<Source> Source::parse(const String &cmdLine,
                           const Path &cwd,
                           const List<Path> &pathEnvironment,
                           List<Path> *unresolvedInputLocations)
{
    assert(cwd.endsWith('/'));
    assert(!unresolvedInputLocations || unresolvedInputLocations->isEmpty());
    String args = cmdLine;
    char quote = '\0';
    List<String> split;
    {
        char *cur = args.data();
        char *prev = cur;
        int size = args.size();
        int escape = 0;
        while (size > 0) {
            switch (*cur) {
            case '"':
            case '\'':
                if (escape % 2 == 0) {
                    if (quote == '\0') {
                        quote = *cur;
                    } else if (*cur == quote) {
                        quote = '\0';
                    }
                }
                escape = 0;
                break;
            case '\\':
                ++escape;
                break;
            case ' ':
                if (quote == '\0') {
                    if (cur > prev)
                        split.append(unquote(trim(prev, cur - prev)));
                    prev = cur + 1;
                }
                escape = 0;
                break;
            default:
                escape = 0;
                break;
            }
            --size;
            ++cur;
        }
        if (cur > prev)
            split.append(trim(prev, cur - prev));
    }
    debug() << "Source::parse (" << args << ") => " << split << cwd;

    for (int i=0; i<split.size(); ++i) {
        if (split.at(i) == "cd" || !findFileInPath(split.at(i), cwd, pathEnvironment).isEmpty()) {
            if (i) {
                split.remove(0, i);
            }
            break;
        }
    }

    if (split.isEmpty()) {
        warning() << "Source::parse No args" << cmdLine;
        return List<Source>();
    }

    Path path;
    if (split.front() == "cd" && split.size() > 3 && split.at(2) == "&&") {
        path = Path::resolved(split.at(1), Path::MakeAbsolute, cwd);
        split.erase(split.begin(), split.begin() + 3);
    } else {
        path = cwd;
    }
    if (split.isEmpty()) {
        warning() << "Source::parse No args" << cmdLine;
        return List<Source>();
    }

    List<Input> inputs;
    Language language = NoLanguage;
    Flags<Flag> sourceFlags;
    List<String> arguments;
    Set<Define> defines;
    List<Include> includePaths;
    int32_t sysRootIndex = -1;
    uint32_t buildRootId = 0;
    Path buildRoot;
    uint32_t compilerId = 0;
    uint64_t includePathHash = 0;
    bool validCompiler = false;

    const int s = split.size();
    String arg;
    Path extraCompiler;
    for (int i=0; i<s; ++i) {
        arg = split.at(i);
        if (arg.isEmpty())
            continue;
        if ((arg.startsWith('\'') && arg.endsWith('\'')) ||
            (arg.startsWith('"') && arg.endsWith('"')))
            arg = arg.mid(1, arg.size() - 2);
        // ### is this even right?
        if (arg.startsWith('-')) {
            if (arg == "-E") {
                warning() << "Preprocessing, ignore" << cmdLine;
                return List<Source>();
            } else if (arg == "-x") {
                const String a = split.value(++i);
                if (a == "c-header") {
                    language = CHeader;
                } else if (a == "c++-header") {
                    language = CPlusPlusHeader;
                } else if (a == "c") {
                    language = C;
                } else if (a == "c++") {
                    language = CPlusPlus;
                } else if (a == "objective-c") {
                    language = ObjectiveC;
                } else if (a == "objective-c++") {
                    language = ObjectiveCPlusPlus;
                } else {
                    return List<Source>();
                }
                arguments.append("-x");
                arguments.append(a);
            } else if (arg.startsWith("-D")) {
                Define define;
                String def, a;
                if (arg.size() == 2) {
                    def = split.value(++i);
                    a = arg + def;
                } else {
                    a = arg;
                    def = arg.mid(2);
                }
                if (!def.isEmpty()) {
                    const int eq = def.indexOf('=');
                    if (eq == -1) {
                        define.define = def;
                    } else {
                        define.define = def.left(eq);
                        define.value = def.mid(eq + 1);
                    }
                    debug("Parsing define: [%s] => [%s]%s[%s]", def.constData(),
                          define.define.constData(),
                          define.value.isEmpty() ? "" : "=",
                          define.value.constData());
                    defines.insert(define);
                }
            } else if (arg.startsWith("-I")) {
                addIncludeArg(includePaths, Source::Include::Type_Include, 2, split, i, path);
#ifdef OS_Darwin
            } else if (arg == "-arch") {
                // Limit -arch to a single format i368/x86_64. Darwin allows
                // mutliple -arch options to build a combined binary.  However,
                // libclang (the indexer) will fail if it gets more than one; it
                // only allows one 'job', in clang parlance, per invocation. It
                // quietly returns a null CXTranslationUnit and is very
                // difficult to see why indexing failed (ie. debug)
                if (!arguments.contains(arg)) {
                    arguments.append(arg);
                    arguments.append(split.value(++i));
                } else {
                    warning() << "[Source::parse] Removing additional -arch argument(s) to allow indexing.";
                }

                // Framework includes
            } else if (arg.startsWith("-F")) {
                addIncludeArg(includePaths, Source::Include::Type_Framework, 2, split, i, path);
            } else if (arg.startsWith("-iframework")) {
                addIncludeArg(includePaths, Source::Include::Type_SystemFramework, 11, split, i, path);
#endif
            } else if (arg.startsWith("-include")) {
                addIncludeArg(includePaths, Source::Include::Type_FileInclude, 8, split, i, path);
            } else if (arg.startsWith("-include-pch")) {
                addIncludeArg(includePaths, Source::Include::Type_FileInclude, 8, split, i, path);
            } else if (arg.startsWith("-isystem")) {
                addIncludeArg(includePaths, Source::Include::Type_System, 8, split, i, path);
            } else if (arg.startsWith("-iquote")) {
                addIncludeArg(includePaths, Source::Include::Type_FileInclude, 7, split, i, path);
            } else if (arg.startsWith("-cxx-isystem")) {
                addIncludeArg(includePaths, Source::Include::Type_System, 12, split, i, path);
            } else if (arg == "-ObjC++") {
                language = ObjectiveCPlusPlus;
                arguments.append(arg);
            } else if (arg == "-ObjC") {
                language = ObjectiveC;
                arguments.append(arg);
            } else if (arg == "-fno-rtti") {
                sourceFlags |= NoRtti;
                arguments.append(arg);
            } else if (arg == "-m32") {
                sourceFlags |= M32;
                arguments.append(arg);
            } else if (arg == "-m64") {
                sourceFlags |= M64;
                arguments.append(arg);
            } else if (arg == "-frtti") {
                sourceFlags &= ~NoRtti;
                arguments.append(arg);
            } else if (arg.startsWith("-std=")) {
                arguments.append(arg);
                // error() << "Got std" << arg;
                if (arg == "-std=c++0x" || arg == "-std=c++11" || arg == "-std=gnu++0x" || arg == "-std=gnu++11") {
                    if (language == CPlusPlusHeader) {
                        language = CPlusPlus11Header;
                    } else {
                        language = CPlusPlus11;
                    }
                }
            } else if (arg.startsWith("-isysroot")) {
                arguments.append(arg);
                if (i + 1 < s) {
                    sysRootIndex = arguments.size();
                    Path root = split.value(++i);
                    root.resolve();
                    arguments.append(root);
                }
            } else if (arg == "-o") {
                if (i + 1 < s) {
                    bool ok;
                    Path p = Path::resolved(split.value(++i), Path::RealPath, path, &ok);
                    // error() << p << ok << split.value(i) << Path::resolved(split.value(i), Path::MakeAbsolute);
                    if (!ok && !p.isAbsolute()) {
                        p.prepend(path); // the object file might not exist
                        p.canonicalize();
                    }
                    buildRoot = RTags::findProjectRoot(p, RTags::BuildRoot);
                    buildRoot.resolve(Path::RealPath, cwd);
                    if (buildRoot.isDir()) {
                        buildRootId = Location::insertFile(buildRoot);
                    } else {
                        buildRoot.clear();
                    }
                }
            } else {
                arguments.append(arg);
                if (hasValue(arg)) {
                    arguments.append(Path::resolved(split.value(++i), Path::MakeAbsolute, path));
                }
            }
        } else {
            bool add = true;
            Path resolved;
            if (!compilerId) {
                add = false;
                const Path compiler = resolveCompiler(arg, cwd, pathEnvironment);
                if (!access(compiler.nullTerminated(), R_OK | X_OK)) {
                    validCompiler = isCompiler(compiler);
                    compilerId = Location::insertFile(compiler);
                } else {
                    break;
                }
            } else {
                const Path c = arg;
                resolved = Path::resolved(arg, Path::RealPath, cwd);
                if (!resolved.isHeader() && !resolved.isSource()) {
                    add = false;
                    if (i == 1) {
                        const Path inPath = findFileInPath(c, cwd, pathEnvironment);
                        if (!access(inPath.nullTerminated(), R_OK | X_OK)) {
                            extraCompiler = inPath;
                            if (!validCompiler)
                                validCompiler = isCompiler(extraCompiler);
                        }
                    }
                }
            }
            if (add) {
                const Language lang = language != NoLanguage ? language : guessLanguageFromSourceFile(resolved);
                if (lang != NoLanguage) {
                    inputs.append({resolved, Path::resolved(arg, Path::MakeAbsolute, cwd), lang});
                } else {
                    warning() << "Can't figure out language for" << arg;
                }
            }
        }
    }

    if (!validCompiler) {
        warning() << "Source::parse Nothing looks like a compiler" << Location::path(compilerId) << extraCompiler;
        return List<Source>();
    }

    if (inputs.isEmpty()) {
        warning() << "Source::parse No file for" << cmdLine;
        return List<Source>();
    }

    List<Source> ret;
    if (!inputs.isEmpty()) {
        if (!buildRootId) {
            buildRoot = RTags::findProjectRoot(inputs.first().realPath, RTags::BuildRoot);
            buildRoot.resolve(Path::RealPath, cwd);
            buildRootId = Location::insertFile(buildRoot);
        }
        includePathHash = ::hashIncludePaths(includePaths, buildRoot);

        ret.resize(inputs.size());
        int idx = 0;
        for (const auto input : inputs) {
            unresolvedInputLocations->append(input.absolute);
            Source &source = ret[idx++];
            source.directory = path;
            source.fileId = Location::insertFile(input.realPath);
            source.extraCompiler = extraCompiler;
            source.compilerId = compilerId;
            source.buildRootId = buildRootId;
            source.includePathHash = includePathHash;
            source.flags = sourceFlags;
            source.defines = defines;
            source.includePaths = includePaths;
            source.arguments = arguments;
            source.sysRootIndex = sysRootIndex;
            source.language = input.language;
            assert(source.language != NoLanguage);
        }
    }
    if (testLog(LogLevel::Warning))
        warning() << "Parsed Source(s) successfully:" << ret;
    return ret;
}
// returns false if at end
static inline bool advance(Set<Source::Define>::const_iterator &it, const Set<Source::Define>::const_iterator end)
{
    while (it != end) {
        if (it->define != "NDEBUG")
            return true;
        ++it;
    }
    return false;
}

static inline bool compareDefinesNoNDEBUG(const Set<Source::Define> &l, const Set<Source::Define> &r)
{
    auto lit = l.begin();
    auto rit = r.begin();
    while (true) {
        if (!advance(lit, l.end())) {
            if (advance(rit, r.end()))
                return false;
            break;
        } else if (!advance(rit, r.end())) {
            return false;
        }

        if (*lit != *rit) {
            return false;
        }
        ++lit;
        ++rit;
    }
    return true;
}

static bool nextArg(List<String>::const_iterator &it,
                    const List<String>::const_iterator end,
                    bool separateDebugAndRelease)
{
    while (it != end) {
        if (isBlacklisted(*it)) {
            const bool val = hasValue(*it);
            ++it;
            if (val && it != end)
                ++it;
        } else if (!separateDebugAndRelease && (*it == "-g" || it->startsWith("-O"))) {
            ++it;
        } else {
            break;
        }
    }
    return it != end;
}

bool Source::compareArguments(const Source &other) const
{
    assert(fileId == other.fileId);

    if  (includePathHash != other.includePathHash) {
        return false;
    }

    const Server::Options *opts = serverOptions();
    const bool separateDebugAndRelease = opts && opts->options & Server::SeparateDebugAndRelease;
    if (separateDebugAndRelease) {
        if (defines != other.defines) {
            return false;
        }
    } else if (!compareDefinesNoNDEBUG(defines, other.defines)) {
        return false;
    }

    auto me = arguments.begin();
    const auto myEnd = arguments.end();
    auto him = other.arguments.begin();
    const auto hisEnd = other.arguments.end();

    while (me != him) {
        if (!nextArg(me, myEnd, separateDebugAndRelease))
            break;
        if (!nextArg(him, hisEnd, separateDebugAndRelease))
            return false;
        if (*me != *him) {
            return false;
        }
        ++me;
        ++him;
    }
    if (him == hisEnd) {
        return true;
    } else if (!nextArg(him, hisEnd, separateDebugAndRelease)) {
        return true;
    }
    return false;
}

List<String> Source::toCommandLine(Flags<CommandLineFlag> flags, bool *usedPch) const
{
    if (usedPch)
        *usedPch = false;
    const Server::Options *options = serverOptions();
    if (!options)
        flags |= (ExcludeDefaultArguments|ExcludeDefaultDefines|ExcludeDefaultIncludePaths);

    List<String> ret;
    ret.reserve(64);
    if ((flags & IncludeCompiler) == IncludeCompiler) {
        ret.append(compiler());
    }
    if (flags & IncludeExtraCompiler && !extraCompiler.isEmpty()) {
        ret.append(extraCompiler);
    }

    Map<String, String> config;
    Set<String> remove;
    if (flags & IncludeRTagsConfig) {
        config = RTags::rtagsConfig(sourceFile());
        remove = config.value("remove-arguments").split(";").toSet();
    }

    for (int i=0; i<arguments.size(); ++i) {
        const String &arg = arguments.at(i);
        const bool hasValue = ::hasValue(arg);
        bool skip = false;
        if (flags & FilterBlacklist && isBlacklisted(arg)) {
            skip = true;
        }
        if (!skip && remove.contains(arg))
            skip = true;
        if (!skip) {
            ret.append(arg);
            if (hasValue)
                ret.append(arguments.value(++i));
        } else if (hasValue) {
            ++i;
        }
    }
    if (!(flags & ExcludeDefaultArguments)) {
        for (const auto &arg : options->defaultArguments)
            ret.append(arg);
    }

    if (flags & IncludeDefines) {
        for (const auto &def : defines)
            ret += def.toString(flags);
        if (!(flags & ExcludeDefaultIncludePaths)) {
            for (const auto &def : options->defines)
                ret += def.toString(flags);
        }
    }
    if (flags & IncludeIncludepaths) {
        for (const auto &inc : includePaths) {
            switch (inc.type) {
            case Source::Include::Type_None:
                assert(0 && "Impossible impossibility");
                break;
            case Source::Include::Type_Include:
                ret << ("-I" + inc.path);
                break;
            case Source::Include::Type_Framework:
                ret << ("-F" + inc.path);
                break;
            case Source::Include::Type_System:
                ret << "-isystem" << inc.path;
                break;
            case Source::Include::Type_SystemFramework:
                ret << "-iframework" << inc.path;
                break;
            case Source::Include::Type_FileInclude:
                if (inc.isPch()) {
                    if (flags & PCHEnabled) {
                        if (usedPch)
                            *usedPch = true;
                        ret << "-include-pch" << (inc.path + ".gch");
                    }
                } else if (inc.path.exists()) {
                    ret << "-include" << inc.path;
                }
                break;
            }
        }
        if (!(flags & ExcludeDefaultIncludePaths)) {
            for (const auto &inc : options->includePaths) {
                switch (inc.type) {
                case Source::Include::Type_None:
                    assert(0 && "Impossible impossibility");
                    break;
                case Source::Include::Type_Include:
                    ret << ("-I" + inc.path);
                    break;
                case Source::Include::Type_Framework:
                    ret << ("-F" + inc.path);
                    break;
                case Source::Include::Type_System:
                    ret << "-isystem" << inc.path;
                    break;
                case Source::Include::Type_SystemFramework:
                    ret << "-iframework" << inc.path;
                    break;
                case Source::Include::Type_FileInclude:
                    ret << "-include" << inc.path;
                    break;
                }
            }
        }
    }
    if (flags & IncludeRTagsConfig) {
        ret << config.value("add-arguments").split(' ');
    }

    if (flags & IncludeSourceFile)
        ret.append(sourceFile());

    return ret;
}

bool Source::Include::isPch() const
{
    for (const char *suffix : { ".gch", ".pch" }) {
        const Path p = path + suffix;
        if (p.isFile()) {
            return true;
        }
    }
    return false;
}

