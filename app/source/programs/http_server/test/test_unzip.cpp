#include <filesystem>


#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <zip.h>


bool UnzipToDir(const std::filesystem::path& zip_path, const std::filesystem::path& output_dir)
{
    auto zip_path_str = zip_path.string();
    auto output_dir_str = output_dir.string();
    int err = 0;
    zip* za = zip_open(zip_path_str.c_str(), ZIP_RDONLY, &err);
    if (!za) return false;

    zip_int64_t num_entries = zip_get_num_entries(za, 0);
    for (zip_int64_t i = 0; i < num_entries; ++i)
    {
        struct zip_stat st;
        zip_stat_init(&st);
        zip_stat_index(za, i, 0, &st);

        zip_file* zf = zip_fopen_index(za, i, 0);
        if (!zf) continue;

        std::vector<char> buffer(st.size);
        zip_fread(zf, buffer.data(), st.size);

        std::ofstream out(output_dir_str + "/" + st.name, std::ios::binary);
        out.write(buffer.data(), static_cast<std::streamsize>(st.size));
        zip_fclose(zf);
    }

    return zip_close(za) == 0;
}


int main()
{
    std::filesystem::path output_dir{"./assets/test_unzip"};
    if (!std::filesystem::exists(output_dir))
    {
        std::filesystem::create_directory(output_dir);
    }
    return UnzipToDir("assets/test.zip", output_dir) ? 0 : -1;
}
