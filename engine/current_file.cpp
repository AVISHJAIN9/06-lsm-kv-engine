#include "current_file.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace lsm {

Status SetCurrentFile(const std::string& dbname, uint64_t manifest_file_number) {
    std::ostringstream oss;
    oss << "MANIFEST-" << std::setw(6) << std::setfill('0') << manifest_file_number;
    std::string manifest_name = oss.str();
    std::string contents = manifest_name + "\n";

    std::string tmp_path = dbname + "/CURRENT.tmp";
    std::string current_path = dbname + "/CURRENT";

    // 1. Write to temporary file
    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return Status::IOError("Failed to create temporary CURRENT file: " + tmp_path);
    }

    ssize_t written = ::write(fd, contents.data(), contents.size());
    if (written != static_cast<ssize_t>(contents.size())) {
        ::close(fd);
        ::unlink(tmp_path.c_str());
        return Status::IOError("Failed to write temporary CURRENT file: " + tmp_path);
    }

    // 2. Fsync temporary file
#ifdef __APPLE__
    ::fcntl(fd, F_FULLFSYNC);
#else
    ::fdatasync(fd);
#endif
    ::close(fd);

    // 3. Atomic rename
    if (::rename(tmp_path.c_str(), current_path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        return Status::IOError("Failed to rename temporary CURRENT file to CURRENT");
    }

    // 4. Fsync parent directory
    int dir_fd = ::open(dbname.c_str(), O_RDONLY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }

    return Status::OK();
}

Status ReadCurrentFile(const std::string& dbname, std::string* out_manifest_filename) {
    if (!out_manifest_filename) {
        return Status::InvalidArgument("Null destination for ReadCurrentFile");
    }

    std::string current_path = dbname + "/CURRENT";
    std::ifstream file(current_path);
    if (!file.is_open()) {
        return Status::NotFound("CURRENT file not found in: " + dbname);
    }

    std::string line;
    if (!std::getline(file, line)) {
        return Status::Corruption("CURRENT file is empty in: " + dbname);
    }

    // Strip trailing newline / carriage return / spaces
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }

    if (line.rfind("MANIFEST-", 0) != 0) {
        return Status::Corruption("CURRENT file content does not start with MANIFEST-: " + line);
    }

    *out_manifest_filename = line;
    return Status::OK();
}

} // namespace lsm
