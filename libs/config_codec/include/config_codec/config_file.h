#ifndef CONFIG_CODEC_CONFIG_FILE_H_
#define CONFIG_CODEC_CONFIG_FILE_H_

#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <yaml-cpp/yaml.h>

namespace config_codec
{

// Replaces `path` with `contents`, or leaves it exactly as it was.
//
// Written to a temporary beside the target, flushed to disk, then renamed over
// it. A plain ofstream that fails mid-write -- a full disk, a pulled card --
// leaves a TRUNCATED file, and a truncated layout is the file the car boots
// from. The target's permissions are kept; a new file gets 0644, because
// mkstemp's 0600 would make a layout saved by one user unreadable to the
// dashboard running as another.
//
// Returns why it failed, or nullopt. The temporary is removed on every failure.
inline std::optional<std::string> writeFileAtomically(const std::string& path,
                                                      std::string_view contents)
{
    std::string temp = path + ".XXXXXX";
    const int fd = ::mkstemp(temp.data());
    if (fd < 0)
    {
        return "could not create a temporary beside it: " + std::string(std::strerror(errno));
    }

    const auto fail = [&](const char* what, bool open) {
        const std::string why = std::string(what) + ": " + std::strerror(errno);
        if (open)
        {
            ::close(fd);
        }
        ::unlink(temp.c_str());
        return std::optional<std::string>(why);
    };

    struct stat existing{};
    const mode_t mode = ::stat(path.c_str(), &existing) == 0 ? (existing.st_mode & 07777) : 0644;
    if (::fchmod(fd, mode) != 0)
    {
        return fail("could not set its permissions", true);
    }

    std::size_t written = 0;
    while (written < contents.size())
    {
        const ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return fail("write failed", true);
        }
        written += static_cast<std::size_t>(n);
    }

    // Before the rename: on ext4 a rename can reach the disk ahead of the data,
    // and a power cut then leaves a zero-length file under the real name.
    if (::fsync(fd) != 0)
    {
        return fail("could not flush it to disk", true);
    }
    if (::close(fd) != 0)
    {
        return fail("close failed", false);
    }
    if (::rename(temp.c_str(), path.c_str()) != 0)
    {
        return fail("could not move it into place", false);
    }
    return std::nullopt;
}

// A YAML document, emitted and written atomically, with a trailing newline.
inline std::optional<std::string> writeYamlFile(const YAML::Node& node, const std::string& path)
{
    YAML::Emitter emitter;
    emitter << node;
    if (!emitter.good())
    {
        return "could not emit YAML: " + emitter.GetLastError();
    }
    return writeFileAtomically(path, std::string(emitter.c_str()) + "\n");
}

}  // namespace config_codec

#endif  // CONFIG_CODEC_CONFIG_FILE_H_
