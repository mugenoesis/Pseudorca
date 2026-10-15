#include "ImageZip.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <boost/log/trivial.hpp>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/miniz_extension.hpp" // IWYU pragma: keep

namespace Slic3r {

namespace {

// A nearest neighbour copy of a thumbnail at the given size, keeping its bottom to top row order.
void add_preview(Zipper &zipper, const char *name, const ThumbnailData &t, size_t w, size_t h)
{
    std::vector<unsigned char> px(w * h * 4);
    for (size_t y = 0; y < h; ++y) {
        const size_t sy = std::min<size_t>(t.height - 1, y * t.height / h);
        for (size_t x = 0; x < w; ++x) {
            const size_t sx = std::min<size_t>(t.width - 1, x * t.width / w);
            std::copy_n(&t.pixels[(sy * t.width + sx) * 4], 4, &px[(y * w + x) * 4]);
        }
    }
    size_t png_size = 0;
    void *png = tdefl_write_image_to_png_file_in_memory_ex(px.data(), int(w), int(h), 4, &png_size, MZ_DEFAULT_LEVEL, 1);
    if (png) {
        zipper.add_entry(name, static_cast<const std::uint8_t *>(png), png_size);
        mz_free(png);
    }
}

} // namespace

void PhrozenZipArchive::export_print(const std::string     fname,
                                     const SLAPrint       &/*print*/,
                                     const ThumbnailsList &thumbnails,
                                     const std::string    &/*projectname*/)
{
    try {
        Zipper zipper{fname, Zipper::FAST_COMPRESSION};
        if (! thumbnails.empty() && thumbnails.front().is_valid()) {
            add_preview(zipper, "preview.png", thumbnails.front(), 954, 850);
            add_preview(zipper, "preview_cropping.png", thumbnails.front(), 168, 150);
        }
        size_t i = 1;
        for (const sla::EncodedRaster &rst : m_layers)
            zipper.add_entry((std::to_string(i++) + "." + rst.extension()).c_str(), rst.data(), rst.size());
        zipper.finalize();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << e.what();
        throw;
    }
}

} // namespace Slic3r
