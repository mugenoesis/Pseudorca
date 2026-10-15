#ifndef SLIC3R_FORMAT_TINYMAKER_HPP
#define SLIC3R_FORMAT_TINYMAKER_HPP

// TinyMaker (open source MSLA printer, github.com/TinyMaker3D/TinyMaker-Open-Source-3D-Printer) reads a folder of
// numbered PNG images from its SD card, one folder per job: 1.png, 2.png, ... The exposure, lift and layer height
// are set on the device, so the images are all there is to a job. This archive is a zip holding that folder,
// to be unpacked onto the card.

#include "SL1.hpp"

namespace Slic3r {

class TinyMakerArchive : public SL1Archive
{
public:
    using SL1Archive::SL1Archive;

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_TINYMAKER_HPP
