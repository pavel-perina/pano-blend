#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// End-to-end CLI tests: spawn the pano-blend binary (PANOBLEND_BIN, injected
// by CMake) against test-data and assert on exit status, output text and
// produced files. Covers the enblend-compat parsing that broke Hugin
// integration: @response files and -w [MODE].

namespace fs = std::filesystem;

namespace {

const std::string kBin = PANOBLEND_BIN;
const std::string kP1  = TEST_DATA_DIR "/p1.tif";
const std::string kP2  = TEST_DATA_DIR "/p2.tif";

struct RunResult {
    bool        ok;      // exit status 0
    std::string output;  // combined stdout+stderr
};

fs::path tempDir() {
    const fs::path d = fs::temp_directory_path() / "pano-blend-cli-tests";
    fs::create_directories(d);
    return d;
}

RunResult run(const std::string& args) {
    const fs::path log = tempDir() / "run.log";
    const std::string cmd =
        kBin + " " + args + " > \"" + log.string() + "\" 2>&1";
    const int rc = std::system(cmd.c_str());
    std::ifstream f(log);
    return { rc == 0,
             { std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>() } };
}

} // namespace

TEST(Cli, VersionPrintsAndExitsCleanly) {
    const RunResult r = run("--version");
    EXPECT_TRUE(r.ok);
    EXPECT_NE(r.output.find("pano-blend"), std::string::npos);
}

TEST(Cli, WrapModeIsConsumedNotTreatedAsInput) {
    const fs::path mask = tempDir() / "wrap_mask.tif";
    fs::remove(mask);
    const RunResult r =
        run(kP1 + " " + kP2 + " -w both -SeamMaskOnly \"" + mask.string() + "\"");
    EXPECT_TRUE(r.ok) << r.output;
    EXPECT_NE(r.output.find("wrap blending not implemented"), std::string::npos);
    EXPECT_TRUE(fs::exists(mask));
}

TEST(Cli, FilenameAfterBareWrapStaysAnInput) {
    const fs::path mask = tempDir() / "barew_mask.tif";
    fs::remove(mask);
    const RunResult r =
        run(kP1 + " -w " + kP2 + " -SeamMaskOnly \"" + mask.string() + "\"");
    EXPECT_TRUE(r.ok) << r.output;  // fails if p2 was eaten (needs 2 inputs)
    EXPECT_TRUE(fs::exists(mask));
}

TEST(Cli, ResponseFileExpandsWithCommentsAndCrlf) {
    const fs::path rsp  = tempDir() / "args.txt";
    const fs::path mask = tempDir() / "rsp_mask.tif";
    fs::remove(mask);
    {
        std::ofstream f(rsp, std::ios::binary);
        f << "# response file written the way Hugin does on Windows\r\n"
          << "\r\n"
          << kP1 << "\r\n"
          << kP2 << "\r\n";
    }
    const RunResult r =
        run("@" + rsp.string() + " -SeamMaskOnly \"" + mask.string() + "\"");
    EXPECT_TRUE(r.ok) << r.output;
    EXPECT_TRUE(fs::exists(mask));
}

TEST(Cli, MissingResponseFileFailsWithClearError) {
    const RunResult r = run("@" + (tempDir() / "no-such-file.txt").string());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("cannot open response file"), std::string::npos);
}
