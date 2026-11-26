// MMX — Acceptance test #11 (spec §10):
// No `float` / `double` may survive in the synthesizable HLS path.
// Args: list of files to scan. We strip `// …` comments and `/* … */` blocks
// before searching for the forbidden tokens.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

static std::string strip_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool in_block = false, in_line = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        char n = i + 1 < src.size() ? src[i + 1] : '\0';
        if (in_block) {
            if (c == '*' && n == '/') { in_block = false; ++i; }
            else if (c == '\n') out.push_back('\n');  // preserve line numbers
        } else if (in_line) {
            if (c == '\n') { in_line = false; out.push_back('\n'); }
        } else {
            if (c == '/' && n == '/') in_line = true;
            else if (c == '/' && n == '*') { in_block = true; ++i; }
            else out.push_back(c);
        }
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file>...\n", argv[0]);
        return 1;
    }
    std::regex pat(R"(\b(float|double)\b)");
    int fails = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i]);
        if (!f) {
            std::fprintf(stderr, "open fail: %s\n", argv[i]);
            return 2;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string code = strip_comments(ss.str());

        std::size_t lineno = 1;
        std::size_t start = 0;
        for (std::size_t p = 0; p <= code.size(); ++p) {
            if (p == code.size() || code[p] == '\n') {
                std::string line = code.substr(start, p - start);
                if (std::regex_search(line, pat)) {
                    std::fprintf(stderr,
                        "%s:%zu: forbidden token in synthesizable code: %s\n",
                        argv[i], lineno, line.c_str());
                    ++fails;
                }
                ++lineno;
                start = p + 1;
            }
        }
    }
    if (fails) {
        std::fprintf(stderr, "FAIL test_no_floating_point_in_fpga: %d hit(s)\n",
                     fails);
        return 3;
    }
    std::printf("PASS test_no_floating_point_in_fpga\n");
    return 0;
}
