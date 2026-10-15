#ifndef SLIC3R_FORMAT_IMAGEZIP_HPP
#define SLIC3R_FORMAT_IMAGEZIP_HPP

// The Phrozen zip: the layer images 1.png, 2.png, ... and the two previews in a zip, and nothing else. The printer
// holds the exposure, lift and layer settings itself (as UVtools writes it, ChituboxZipFile.cs without run.gcode).

#include "SL1.hpp"

namespace Slic3r {

class PhrozenZipArchive : public SL1Archive
{
public:
    using SL1Archive::SL1Archive;

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_IMAGEZIP_HPP
