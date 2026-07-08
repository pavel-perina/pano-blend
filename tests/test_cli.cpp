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

// The Pass-1/Pass-2 split: a blend driven by a saved label map must be
// byte-identical to the direct single-run blend.
TEST(Cli, LabelMapRoundTripBlendsIdentically) {
    const fs::path mask   = tempDir() / "rt_mask.tif";
    const fs::path direct = tempDir() / "rt_direct.tif";
    const fs::path viamap = tempDir() / "rt_viamap.tif";
    for (const auto& p : {mask, direct, viamap}) fs::remove(p);

    ASSERT_TRUE(run(kP1 + " " + kP2 + " -o \"" + direct.string() + "\"").ok);
    ASSERT_TRUE(run(kP1 + " " + kP2 + " -SeamMaskOnly \"" + mask.string() + "\"").ok);
    ASSERT_TRUE(run(kP1 + " " + kP2 + " -LoadLabelMap \"" + mask.string() +
                    "\" -o \"" + viamap.string() + "\"").ok);

    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string{ std::istreambuf_iterator<char>(f),
                            std::istreambuf_iterator<char>() };
    };
    const std::string a = slurp(direct), b = slurp(viamap);
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a, b);
}

TEST(Cli, LoadLabelMapRejectsNonLabelImage) {
    // p1 as a "label map": right size (it is canvas-sized), but its photo
    // values decode to labels far beyond the input count.
    const RunResult r = run(kP1 + " " + kP2 + " -LoadLabelMap " + kP1 +
                            " -o /dev/null");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("inputs"), std::string::npos);
}

// --levels must actually reach the blender: a 1-level pyramid feathers the
// seam far narrower than the 5-level default, so the outputs must differ.
TEST(Cli, LevelsFlagChangesTheBlend) {
    const fs::path deflt = tempDir() / "lv_default.tif";
    const fs::path one   = tempDir() / "lv_one.tif";
    ASSERT_TRUE(run(kP1 + " " + kP2 + " -o \"" + deflt.string() + "\"").ok);
    ASSERT_TRUE(run(kP1 + " " + kP2 + " --levels=1 -o \"" + one.string() + "\"").ok);

    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string{ std::istreambuf_iterator<char>(f),
                            std::istreambuf_iterator<char>() };
    };
    const std::string a = slurp(deflt), b = slurp(one);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    EXPECT_NE(a, b);
}

TEST(Cli, LevelsFlagValidatesRange) {
    const RunResult r = run(kP1 + " " + kP2 + " -l 0 -o /dev/null");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("expected 1..29"), std::string::npos);
}

TEST(Cli, MissingResponseFileFailsWithClearError) {
    const RunResult r = run("@" + (tempDir() / "no-such-file.txt").string());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("cannot open response file"), std::string::npos);
}
