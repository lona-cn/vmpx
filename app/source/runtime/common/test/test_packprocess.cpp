#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

#include "VMPX.h"

namespace
{
    bool WriteText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        return static_cast<bool>(output);
    }

    bool RunCase(const std::filesystem::path& executable, const std::filesystem::path& root,
                 std::string_view mode, vmpx::PackProcessOptions options, bool expected_success)
    {
        const auto case_dir = root / mode;
        std::filesystem::create_directories(case_dir);
        const auto input = case_dir / "input.exe";
        const auto project = case_dir / "project.vmp";
        const auto output_dir = case_dir / "output";
        std::filesystem::create_directories(output_dir);
        if (!WriteText(input, "input") ||
            !WriteText(project, std::format(
                "<Document><Protection InputFileName=\"input.exe\" OutputFileName=\"packed.exe\"/>"
                "<Mode>{}</Mode></Document>", mode)))
            return false;

        auto result = vmpx::PackApp(executable, project, output_dir, options);
        if (static_cast<bool>(result) != expected_success)
        {
            std::cerr << "unexpected result for " << mode;
            if (!result) std::cerr << ": " << result.error();
            std::cerr << '\n';
            return false;
        }
        if (expected_success)
        {
            std::ifstream packed(*result, std::ios::binary);
            std::string contents{std::istreambuf_iterator<char>{packed}, {}};
            return contents == "packed";
        }
        return true;
    }
}

int main(int argc, char* argv[])
{
    if (argc == 3)
    {
        std::ifstream project(argv[1], std::ios::binary);
        std::string contents{std::istreambuf_iterator<char>{project}, {}};
        if (contents.find("<Mode>timeout</Mode>") != std::string::npos)
            std::this_thread::sleep_for(std::chrono::seconds{5});
        if (contents.find("<Mode>output</Mode>") != std::string::npos)
        {
            std::cout << std::string(32, 'x') << std::flush;
            return 0;
        }
        if (contents.find("<Mode>missing-output</Mode>") != std::string::npos)
        {
            std::cout << "Compilation completed" << std::flush;
            return 0;
        }
        if (contents.find("<Mode>failure</Mode>") != std::string::npos)
        {
            std::cout << "Compilation completed" << std::flush;
            return 7;
        }
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        output << "packed";
        std::cout << "Compilation completed" << std::flush;
        return output ? 0 : 8;
    }

    const auto root = std::filesystem::temp_directory_path() /
        ("vmpx-pack-process-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{root};

    const auto executable = std::filesystem::absolute(argv[0]);
    vmpx::PackProcessOptions options{};
    options.timeout = std::chrono::seconds{2};
    if (!RunCase(executable, root, "success", options, true)) return 1;

    options.timeout = std::chrono::milliseconds{100};
    if (!RunCase(executable, root, "timeout", options, false)) return 2;

    options.timeout = std::chrono::seconds{2};
    options.max_stdout_bytes = 8;
    if (!RunCase(executable, root, "output", options, false)) return 3;
    options.max_stdout_bytes = 64;
    if (!RunCase(executable, root, "failure", options, false)) return 4;
    if (!RunCase(executable, root, "missing-output", options, false)) return 5;
    return 0;
}
