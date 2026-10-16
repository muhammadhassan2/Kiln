#include "shader/Translate.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace fs = std::filesystem;

namespace shader {

namespace {

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

inline bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
inline bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
inline size_t skipSpace(std::string_view s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return i;
}
// Reads an identifier at i (after optional spaces); returns empty view if none.
std::string_view readIdent(std::string_view s, size_t& i) {
    i = skipSpace(s, i);
    const size_t b = i;
    if (i < s.size() && isIdentStart(s[i]))
        while (i < s.size() && isIdentChar(s[i])) ++i;
    return s.substr(b, i - b);
}
// "#  word" at the start of a line: returns the directive word ("version", "include", ...).
std::string_view directive(std::string_view line, size_t& i) {
    i = skipSpace(line, 0);
    if (i >= line.size() || line[i] != '#') return {};
    ++i;
    return readIdent(line, i);
}

// Whole-source facts, gathered in one pass over the identifiers.
struct SourceScan {
    bool hasMain = false, hasMainImage = false, hasTransition = false;
    std::unordered_set<std::string> uniforms;  // names declared with "uniform [precision] type name"
};

bool isPrecision(std::string_view w) { return w == "highp" || w == "mediump" || w == "lowp"; }

SourceScan scanSource(std::string_view s) {
    SourceScan r;
    std::string_view p3, p2, p1;  // the identifiers before the current one (reset by punctuation)
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (!isIdentStart(c)) {
            if (isIdentChar(c)) {  // number literal
                while (i < s.size() && isIdentChar(s[i])) ++i;
                p3 = p2 = p1 = {};
            } else {
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') p3 = p2 = p1 = {};
                ++i;
            }
            continue;
        }
        const size_t b = i;
        while (i < s.size() && isIdentChar(s[i])) ++i;
        const std::string_view id = s.substr(b, i - b);
        const size_t after = skipSpace(s, i);
        if (after < s.size() && s[after] == '(') {
            if (id == "main" && p1 == "void") r.hasMain = true;
            else if (id == "mainImage") r.hasMainImage = true;
            else if (id == "transition" && p1 == "vec4") r.hasTransition = true;
        }
        if ((p2 == "uniform" && !isPrecision(p1)) || (p3 == "uniform" && isPrecision(p2)))
            r.uniforms.emplace(id);
        p3 = p2;
        p2 = p1;
        p1 = id;
    }
    return r;
}

struct Rewriter {
    bool legacy = false;

    std::string apply(const std::string& line) const {
        size_t k;
        if (directive(line, k) == "extension") {
            std::string_view rest(line);
            if (readIdent(rest, k).substr(0, 7) == "GL_OES_") return "// " + line;  // no GL_OES_* on desktop
        }
        std::string out;
        out.reserve(line.size() + 8);
        const std::string_view s(line);
        size_t i = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (!isIdentStart(c)) {
                if (isIdentChar(c)) {  // number literal: copy as is
                    const size_t b = i;
                    while (i < s.size() && isIdentChar(s[i])) ++i;
                    out.append(s.substr(b, i - b));
                } else {
                    out += c;
                    ++i;
                }
                continue;
            }
            const size_t b = i;
            while (i < s.size() && isIdentChar(s[i])) ++i;
            const std::string_view id = s.substr(b, i - b);
            if (id == "samplerExternalOES") { out += "sampler2D"; continue; }
            if (legacy) {
                if (id == "gl_FragColor") { out += "ss_FragColor"; continue; }
                if (id == "gl_FragData") {  // gl_FragData [ 0 ]
                    size_t j = skipSpace(s, i);
                    if (j < s.size() && s[j] == '[') {
                        j = skipSpace(s, j + 1);
                        if (j < s.size() && s[j] == '0' && (j + 1 >= s.size() || !isIdentChar(s[j + 1]))) {
                            j = skipSpace(s, j + 1);
                            if (j < s.size() && s[j] == ']') {
                                out += "ss_FragColor";
                                i = j + 1;
                                continue;
                            }
                        }
                    }
                }
                if (id == "texture2D" || id == "textureCube") { out += "texture"; continue; }
                if (id == "texture2DProj") { out += "textureProj"; continue; }
                if (id == "texture2DLod" || id == "texture2DLodEXT" || id == "textureCubeLod" ||
                    id == "textureCubeLodEXT") { out += "textureLod"; continue; }
                if (id == "varying") { out += "in"; continue; }
            }
            out.append(id);
        }
        return out;
    }
};

// "<qualifiers> in <precision>? (float|vec2|vec3|vec4) name ;" at the start of a line.
bool parseInDecl(std::string_view l, Varying& v) {
    size_t i = 0;
    std::string_view w = readIdent(l, i);
    while (w == "flat" || w == "smooth" || w == "noperspective" || w == "centroid" || w == "highp" ||
           w == "mediump" || w == "lowp")
        w = readIdent(l, i);
    if (w != "in") return false;
    w = readIdent(l, i);
    if (w == "highp" || w == "mediump" || w == "lowp") w = readIdent(l, i);
    if (w != "float" && w != "vec2" && w != "vec3" && w != "vec4") return false;
    const std::string_view name = readIdent(l, i);
    if (name.empty()) return false;
    i = skipSpace(l, i);
    if (i >= l.size() || l[i] != ';') return false;
    v.type = std::string(w);
    v.name = std::string(name);
    return true;
}

// "#version 300 es" -> 300, true. Anything after the profile is ignored.
bool parseVersion(std::string_view l, int& version, bool& es) {
    size_t i;
    if (directive(l, i) != "version") return false;
    i = skipSpace(l, i);
    const size_t b = i;
    while (i < l.size() && std::isdigit(static_cast<unsigned char>(l[i]))) ++i;
    if (i == b) return false;
    version = std::stoi(std::string(l.substr(b, i - b)));
    es = readIdent(l, i) == "es";
    return true;
}

// '#include "name"' or '#include <name>'.
bool parseInclude(std::string_view l, std::string& name) {
    size_t i;
    if (directive(l, i) != "include") return false;
    i = skipSpace(l, i);
    if (i >= l.size() || (l[i] != '"' && l[i] != '<')) return false;
    const size_t b = ++i;
    while (i < l.size() && l[i] != '"' && l[i] != '>') ++i;
    if (i >= l.size() || i == b) return false;
    name = std::string(l.substr(b, i - b));
    return true;
}

}  // namespace

std::vector<DefineInfo> findOverridableDefines(const std::string& source) {
    std::vector<DefineInfo> out;
    const std::vector<std::string> lines = splitLines(source);
    for (size_t n = 0; n + 1 < lines.size(); ++n) {
        size_t i;
        if (directive(lines[n], i) != "ifndef") continue;
        const std::string_view name = readIdent(lines[n], i);
        if (name.empty()) continue;
        size_t m = n + 1;
        while (m < lines.size() && skipSpace(lines[m], 0) >= lines[m].size()) ++m;  // blank lines
        if (m >= lines.size() || directive(lines[m], i) != "define") continue;
        if (readIdent(lines[m], i) != name) continue;
        std::string value(std::string_view(lines[m]).substr(skipSpace(lines[m], i)));
        if (const size_t c = value.find("//"); c != std::string::npos) value.erase(c);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
        if (value.empty()) continue;
        bool dup = false;
        for (const auto& d : out) dup = dup || d.name == name;
        if (!dup) out.push_back({std::string(name), value, static_cast<int>(m) + 1});
    }
    return out;
}

std::string applyDefineOverrides(const std::string& source, const std::vector<DefineInfo>& defines,
                                 const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::pair<int, std::string>> edits;  // 1-based line -> new text
    for (const auto& d : defines)
        for (const auto& [name, value] : overrides)
            if (d.name == name && d.value != value) edits.push_back({d.line, "#define " + name + " " + value});
    if (edits.empty()) return source;
    std::string out;
    out.reserve(source.size() + 32);
    int line = 1;
    size_t pos = 0;
    while (pos <= source.size()) {
        size_t nl = source.find('\n', pos);
        const bool last = nl == std::string::npos;
        if (last) nl = source.size();
        const std::string* repl = nullptr;
        for (const auto& e : edits)
            if (e.first == line) repl = &e.second;
        if (repl) out += *repl;
        else out.append(source, pos, nl - pos);
        if (last) break;
        out += '\n';
        pos = nl + 1;
        ++line;
    }
    return out;
}

namespace {

class Emitter {
public:
    Emitter(const Rewriter& rw, const TranslateOptions& opts, TranslatedSource& out) : rw_(rw), opts_(opts), out_(out) {}

    // fileIndex 0 = user source. skipLine = version line to blank out (main file only).
    void emit(const std::vector<std::string>& lines, int fileIndex, int depth, int skipLine,
              const fs::path& baseDir) {
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            if (i == skipLine) {
                body_.push_back("");
                continue;
            }
            std::string name;
            if (parseInclude(lines[i], name)) {
                fs::path found;
                std::vector<fs::path> dirs;
                if (!baseDir.empty()) dirs.push_back(baseDir);
                for (const auto& d : opts_.includeDirs) dirs.push_back(d);
                for (const auto& d : dirs) {
                    std::error_code ec;
                    if (fs::exists(d / name, ec)) {
                        found = d / name;
                        break;
                    }
                }
                std::string text;
                if (found.empty() || depth >= 8 || !readFile(found, text)) {
                    out_.includeErrors.push_back("line " + std::to_string(i + 1) + ": cannot find include \"" +
                                                 name + "\"");
                    body_.push_back("// " + lines[i]);
                    continue;
                }
                out_.includes.push_back(found);
                const int idx = static_cast<int>(out_.includes.size());
                body_.push_back("#line 1 " + std::to_string(idx));
                emit(splitLines(text), idx, depth + 1, -1, found.parent_path());
                body_.push_back("#line " + std::to_string(i + 2) + " " + std::to_string(fileIndex));
                continue;
            }
            body_.push_back(rw_.apply(lines[i]));
        }
    }

    const std::vector<std::string>& body() const { return body_; }

private:
    const Rewriter& rw_;
    const TranslateOptions& opts_;
    TranslatedSource& out_;
    std::vector<std::string> body_;
};

}  // namespace

TranslatedSource translateFragment(const std::string& userSource, const TranslateOptions& options) {
    const std::vector<std::string> lines = splitLines(userSource);

    int versionLine = -1, version = 0;
    bool es = false;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (parseVersion(lines[i], version, es)) {
            versionLine = i;
            break;
        }
    }

    const SourceScan scan = scanSource(userSource);
    const bool hasMain = scan.hasMain, hasMainImage = scan.hasMainImage, hasTransition = scan.hasTransition;

    TranslatedSource out;
    if (!hasMain && hasMainImage) out.kind = SourceKind::Shadertoy;
    else if (!hasMain && hasTransition) out.kind = SourceKind::Transition;
    else if (versionLine < 0 || (!es && version < 330) || (es && version < 300)) out.kind = SourceKind::Legacy;
    else if (es) out.kind = SourceKind::Es3;
    else out.kind = SourceKind::Modern;

    switch (out.kind) {
        case SourceKind::Modern: out.dialect = "GLSL " + std::to_string(version); break;
        case SourceKind::Es3: out.dialect = "GLSL ES " + std::to_string(version / 100) + "." +
                                            std::to_string(version % 100 / 10) + "0 (as 410 core)"; break;
        case SourceKind::Legacy: out.dialect = versionLine < 0 || es ? "GLSL ES 1.00 (legacy, translated)"
                                                                     : "GLSL " + std::to_string(version) + " (legacy, translated)"; break;
        case SourceKind::Shadertoy: out.dialect = "Shadertoy (mainImage)"; break;
        case SourceKind::Transition: out.dialect = "gl-transition (transition())"; break;
    }

    Rewriter rw;
    rw.legacy = out.kind == SourceKind::Legacy || out.kind == SourceKind::Shadertoy ||
                out.kind == SourceKind::Transition;

    // ---- header (before any user line) ------------------------------------------------
    std::ostringstream src;
    if (out.kind == SourceKind::Modern) src << lines[static_cast<size_t>(versionLine)] << "\n";
    else src << "#version 410 core\n";

    auto declared = [&](const char* name) { return scan.uniforms.count(name) > 0; };
    if (out.kind == SourceKind::Shadertoy) {
        const char* decls[][2] = {{"iResolution", "vec3"}, {"iTime", "float"},   {"iTimeDelta", "float"},
                                  {"iFrame", "int"},       {"iMouse", "vec4"},   {"iDate", "vec4"},
                                  {"iSampleRate", "float"}, {"iChannel0", "sampler2D"}, {"iChannel1", "sampler2D"},
                                  {"iChannel2", "sampler2D"}, {"iChannel3", "sampler2D"}};
        for (auto& d : decls)
            if (!declared(d[0])) src << "uniform " << d[1] << " " << d[0] << ";\n";
        if (!declared("iChannelResolution")) src << "uniform vec3 iChannelResolution[4];\n";
        if (!declared("iChannelTime")) src << "uniform float iChannelTime[4];\n";
    }
    if (out.kind == SourceKind::Transition) {
        src << "uniform sampler2D u_from;\nuniform sampler2D u_to;\nuniform vec2 u_resolution;\n";
        if (!declared("progress")) src << "uniform float progress;\n";
        if (!declared("ratio")) src << "uniform float ratio;\n";
        src << "vec4 getFromColor(vec2 uv) { return texture(u_from, uv); }\n"
               "vec4 getToColor(vec2 uv) { return texture(u_to, uv); }\n";
    }
    if (rw.legacy) src << "out vec4 ss_FragColor;\n";
    src << "#line 1 0\n";

    // ---- body (line numbers identical to the user's source) -------------------------
    fs::path baseDir = options.includeDirs.empty() ? fs::path() : options.includeDirs.front();
    Emitter em(rw, options, out);
    em.emit(lines, 0, 0, versionLine, baseDir);
    for (const auto& l : em.body()) src << l << "\n";

    // ---- footer ------------------------------------------------------------------------
    if (out.kind == SourceKind::Shadertoy)
        src << "void main() { vec4 c = vec4(0.0, 0.0, 0.0, 1.0); mainImage(c, gl_FragCoord.xy); ss_FragColor = c; }\n";
    if (out.kind == SourceKind::Transition)
        src << "void main() { ss_FragColor = transition(gl_FragCoord.xy / u_resolution); }\n";
    out.source = src.str();

    // ---- fragment inputs the vertex stage must feed ---------------------------------
    for (const auto& l : em.body()) {
        Varying v;
        if (parseInDecl(l, v)) out.varyings.push_back(std::move(v));
    }
    return out;
}

std::string buildVertexShader(const std::vector<Varying>& varyings, bool uvTopLeft) {
    std::ostringstream vs;
    vs << "#version 410 core\n";
    for (const auto& v : varyings) vs << "out " << v.type << " " << v.name << ";\n";
    vs << "void main() {\n"
          "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
          "    vec2 uv = p;\n";
    if (uvTopLeft) vs << "    uv.y = 1.0 - uv.y;\n";
    for (const auto& v : varyings) {
        vs << "    " << v.name << " = ";
        if (v.type == "vec2") vs << "uv";
        else if (v.type == "vec3") vs << "vec3(uv, 0.0)";
        else if (v.type == "vec4") vs << "vec4(uv, 0.0, 1.0)";
        else vs << "uv.x";
        vs << ";\n";
    }
    vs << "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
    return vs.str();
}

}  // namespace shader
