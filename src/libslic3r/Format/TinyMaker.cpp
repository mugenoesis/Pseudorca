#include "TinyMaker.hpp"

#include <algorithm>
#include <string>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Zipper.hpp"

namespace Slic3r {

namespace {

// The firmware opens the job folder by name from its file list, so keep the name plain.
std::string job_folder_name(const std::string &name)
{
    std::string out;
    for (char c : name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return out.empty() ? std::string("job") : out;
}

} // namespace

void TinyMakerArchive::export_print(const std::string     fname,
                                    const SLAPrint       &/*print*/,
                                    const ThumbnailsList &/*thumbnails*/,
                                    const std::string    &projectname)
{
    const std::string folder = job_folder_name(
        projectname.empty() ? boost::filesystem::path(fname).stem().string() : projectname);

    try {
        Zipper zipper{fname, Zipper::FAST_COMPRESSION};
        size_t i = 1;
        for (const sla::EncodedRaster &rst : m_layers)
            zipper.add_entry((folder + "/" + std::to_string(i++) + "." + rst.extension()).c_str(),
                             rst.data(), rst.size());
        zipper.finalize();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << e.what();
        throw;
    }
}

} // namespace Slic3r
