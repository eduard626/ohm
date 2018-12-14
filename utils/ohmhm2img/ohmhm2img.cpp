//
// author Kazys Stepanas
//

#include <ohm/Heightmap.h>
#include <ohm/HeightmapVoxel.h>
#include <ohm/MapSerialise.h>
#include <ohm/OccupancyMap.h>
#include <ohm/Voxel.h>

#include <ohmheightmaputil/HeightmapImage.h>

#include <ohmutil/OhmUtil.h>
#include <ohmutil/PlyMesh.h>
#include <ohmutil/ProgressMonitor.h>
#include <ohmutil/SafeIO.h>
#include <ohmutil/ScopedTimeDisplay.h>

#include <png.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>


namespace
{
  int quit = 0;

  void onSignal(int arg)
  {
    if (arg == SIGINT || arg == SIGTERM)
    {
      ++quit;
    }
  }

  enum ExportMode
  {
    kNormals16 = ohm::HeightmapImage::kImageNormals,
    kNormals8 = ohm::HeightmapImage::kImageNormals888,
    kHeights = ohm::HeightmapImage::kImageHeights,
    kTraversability
  };

  enum ExportImageType
  {
    kExportError = -1,
    kExportRGB8,
    kExportRGB16,
    kExportGrey8,
    kExportGrey16
  };

  struct Options
  {
    std::string map_file;
    std::string image_file;
    ExportMode image_mode = kNormals16;
    ohm::HeightmapImage::NormalsMode normals_mode = ohm::HeightmapImage::kNormalsAverage;
    double traverse_angle = 45.0;

    ohm::HeightmapImage::ImageType imageType() const
    {
      switch (image_mode)
      {
      case kNormals16:
        return ohm::HeightmapImage::kImageNormals;
      case kNormals8:
        return ohm::HeightmapImage::kImageNormals888;
      case kHeights:
        return ohm::HeightmapImage::kImageHeights;
      case kTraversability:
        return ohm::HeightmapImage::kImageNormals;
      default:
        break;
      }

      return ohm::HeightmapImage::kImageNormals;
    }
  };


  class LoadMapProgress : public ohm::SerialiseProgress
  {
  public:
    LoadMapProgress(ProgressMonitor &monitor)
      : monitor_(monitor)
    {}

    bool quit() const override { return ::quit > 1; }

    void setTargetProgress(unsigned target) override { monitor_.beginProgress(ProgressMonitor::Info(target)); }
    void incrementProgress(unsigned inc = 1) override { monitor_.incrementProgressBy(inc); }

  private:
    ProgressMonitor &monitor_;
  };

  ExportImageType convertImage(std::vector<uint8_t> &export_pixels, const uint8_t *raw,
                               const ohm::HeightmapImage::BitmapInfo &info, const Options &opt)
  {
    if (opt.image_mode == kNormals16 && info.type == ohm::HeightmapImage::kImageNormals)
    {
      // Need to convert float colour to u16
      export_pixels.clear();
      export_pixels.reserve(info.image_width * info.image_height * 3 * sizeof(uint16_t));

      float red, green, blue;
      uint16_t red16, green16, blue16;

      const auto convert_colour = [](float c) -> uint16_t { return uint16_t(c * float(0xffffu)); };

      const auto push_channel = [](std::vector<uint8_t> &out, uint16_t c) {
        const size_t insert_index = out.size();
        // Push the stride in bytes.
        for (size_t i = 0; i < sizeof(uint16_t); ++i)
        {
          out.push_back(0);
        }

        *reinterpret_cast<uint16_t *>(&out[insert_index]) = c;
      };

      for (size_t i = 0; i < info.image_width * info.image_height * info.bpp; i += 3 * sizeof(float))
      {
        red = *reinterpret_cast<const float *>(raw + i);
        green = *reinterpret_cast<const float *>(raw + i + sizeof(float));
        blue = *reinterpret_cast<const float *>(raw + i + 2 * sizeof(float));

        red16 = convert_colour(red);
        green16 = convert_colour(green);
        blue16 = convert_colour(blue);

        // No data: black
        if (red * red + green * green + blue * blue < 0.5f)
        {
          red16 = green16 = blue16 = 0;
        }

        push_channel(export_pixels, red16);
        push_channel(export_pixels, green16);
        push_channel(export_pixels, blue16);
      }

      return kExportRGB16;
    }

    if (opt.image_mode == kNormals8 && info.type == ohm::HeightmapImage::kImageNormals888)
    {
      export_pixels.resize(info.image_width * info.image_height * 3);
      memcpy(export_pixels.data(), raw, export_pixels.size());
      return kExportRGB8;
    }

    if (opt.image_mode == kHeights && info.bpp == sizeof(float))
    {
      export_pixels.resize(info.image_width * info.image_height * sizeof(uint16_t));
      const float *depth_pixels = reinterpret_cast<const float *>(raw);
      uint16_t *depth_out = reinterpret_cast<uint16_t *>(export_pixels.data());

      for (size_t i = 0; i < info.image_width * info.image_height; ++i)
      {
        depth_out[i] = uint16_t(1.0f - depth_pixels[i] * float(0xffffu));
      }

      return kExportGrey16;
    }

    if (opt.image_mode == kTraversability && info.type == ohm::HeightmapImage::kImageNormals)
    {
      static_assert(sizeof(glm::vec3) == sizeof(float) * 3, "glm::vec3 mismatch");
      const glm::vec3 *pseudo_normals = reinterpret_cast<const glm::vec3 *>(raw);
      export_pixels.resize(info.image_width * info.image_height);

      const uint8_t c_unknown = 127u;
      const uint8_t c_blocked = 0u;
      const uint8_t c_free = 255u;
      glm::vec3 normal;
      const glm::vec3 flat(0, 0, 1);
      float dot;
      const float free_threshold = float(std::cos(M_PI * opt.traverse_angle / 180.0));

      for (size_t i = 0; i < info.image_width * info.image_height; ++i)
      {
        if (glm::dot(pseudo_normals[i], pseudo_normals[i]) > 0.5f * 0.5f)
        {
          normal = pseudo_normals[i];
          normal = 2.0f * normal - glm::vec3(1.0f);
          normal = glm::normalize(normal);
          dot = glm::dot(normal, flat);
          if (dot >= free_threshold)
          {
            export_pixels[i] = c_free;
          }
          else
          {
            export_pixels[i] = c_blocked;
          }
        }
        else
        {
          // No data.
          export_pixels[i] = c_unknown;
        }
      }

      return kExportGrey8;
    }

    return kExportError;
  }


  bool savePng(const char *filename, const std::vector<uint8_t> &raw, ExportImageType type, unsigned w, unsigned h)
  {
    png_image image;

    // Initialize the 'png_image' structure.
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = int(w);
    image.height = int(h);
    image.flags = 0;
    image.colormap_entries = 0;

    int row_stride = w;
    switch (type)
    {
    case kExportRGB8:
      image.format = PNG_FORMAT_RGB;
      row_stride = -int(w * 3);
      break;
    case kExportRGB16:
      image.format = PNG_FORMAT_RGB | PNG_IMAGE_FLAG_16BIT_sRGB;
      row_stride = -int(w * 3);
      break;
    case kExportGrey8:
      image.format = PNG_FORMAT_GRAY;
      row_stride = -int(w);
      break;
    case kExportGrey16:
      image.format = PNG_FORMAT_LINEAR_Y;
      row_stride = -int(w);
      break;
    default:
      image.format = PNG_FORMAT_GRAY;
      row_stride = -int(w);
      break;
    }

    // Negative row stride to flip the image.
    if (png_image_write_to_file(&image, filename, false,  // convert_to_8bit,
                                raw.data(),
                                row_stride,  // row_stride
                                nullptr      // colormap
                                ))
    {
      return true;
    }

    return false;
  }
}  // namespace


// Custom option parsing. Must come before we include Options.h/cxxopt.hpp
std::istream &operator>>(std::istream &in, ExportMode &mode)
{
  std::string mode_str;
  in >> mode_str;
  if (mode_str.compare("norm8") == 0)
  {
    mode = kNormals8;
  }
  else if (mode_str.compare("norm16") == 0)
  {
    mode = kNormals16;
  }
  else if (mode_str.compare("height") == 0)
  {
    mode = kHeights;
  }
  else if (mode_str.compare("traverse") == 0)
  {
    mode = kTraversability;
  }
  // else
  // {
  //   throw cxxopts::invalid_option_format_error(modeStr);
  // }
  return in;
}

std::ostream &operator<<(std::ostream &out, const ExportMode mode)
{
  switch (mode)
  {
  case kNormals8:
    out << "norm8";
    break;
  case kNormals16:
    out << "norm16";
    break;
  case kHeights:
    out << "height";
    break;
  case kTraversability:
    out << "traverse";
    break;
  }
  return out;
}

std::istream &operator>>(std::istream &in, ohm::HeightmapImage::NormalsMode &mode)
{
  std::string mode_str;
  in >> mode_str;
  if (mode_str.compare("average") == 0 || mode_str.compare("avg") == 0)
  {
    mode = ohm::HeightmapImage::kNormalsAverage;
  }
  else if (mode_str.compare("worst") == 0)
  {
    mode = ohm::HeightmapImage::kNormalsWorst;
  }
  // else
  // {
  //   throw cxxopts::invalid_option_format_error(modeStr);
  // }
  return in;
}

std::ostream &operator<<(std::ostream &out, const ohm::HeightmapImage::NormalsMode mode)
{
  switch (mode)
  {
  case ohm::HeightmapImage::kNormalsAverage:
    out << "average";
    break;
  case ohm::HeightmapImage::kNormalsWorst:
    out << "worst";
    break;
  }
  return out;
}
// Must be after argument streaming operators.
#include <ohmutil/Options.h>

int parseOptions(Options &opt, int argc, char *argv[])
{
  cxxopts::Options optParse(argv[0], "\nCreate a heightmap from an occupancy map.\n");
  optParse.positional_help("<map.ohm> <heightmap.ohm>");

  try
  {
    optParse.add_options()("help", "Show help.")                                       //
      ("i", "The input heightmap file (ohm).", cxxopts::value(opt.map_file))           //
      ("o", "The output heightmap image file (png).", cxxopts::value(opt.image_file))  //
      ("m,mode",
       "The image output mode [norm8, norm16, height, traverse]. norm8 exports a normal map image with 8 bits per "
       "pixel. norm16 "
       "uses 16 bits per pixel. height is a greyscale image where the colour is the relative heights. traverse colours "
       "by traversability black (non-traversable), white (traversable), grey (unknown) based on the --traverse-angle "
       "argument.",
       cxxopts::value(opt.image_mode)->default_value(optStr(opt.image_mode)))  //
      ("traverse-angle", "The maximum traversable angle (degrees) for use with mode=traverse.",
       cxxopts::value(opt.traverse_angle)->default_value(optStr(opt.traverse_angle)))  //
      ("normals",
       "Defines how vertex normals are calculated: [average/avg, worst]. average averages triangle normals, worst "
       "selects the least horizontal triangle normal for a vertex.",
       cxxopts::value(opt.normals_mode)->default_value(optStr(opt.normals_mode)))  //
      ;

    optParse.parse_positional({ "i", "o" });

    cxxopts::ParseResult parsed = optParse.parse(argc, argv);

    if (parsed.count("help") || parsed.arguments().empty())
    {
      // show usage.
      std::cout << optParse.help({ "", "Group" }) << std::endl;
      return 1;
    }

    if (opt.map_file.empty())
    {
      std::cerr << "Missing input map" << std::endl;
      return -1;
    }

    if (opt.image_file.empty())
    {
      std::cerr << "Missing output name" << std::endl;
      return -1;
    }
  }
  catch (const cxxopts::OptionException &e)
  {
    std::cerr << "Argument error\n" << e.what() << std::endl;
    return -1;
  }

  return 0;
}


int main(int argc, char *argv[])
{
  Options opt;

  std::cout.imbue(std::locale(""));

  int res = 0;
  res = parseOptions(opt, argc, argv);

  if (res)
  {
    return res;
  }

  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);

  printf("Loading map %s\n", opt.map_file.c_str());
  ProgressMonitor prog(10);
  LoadMapProgress load_progress(prog);
  ohm::Heightmap heightmap;
  ohm::MapVersion version;

  prog.setDisplayFunction([&opt](const ProgressMonitor::Progress &prog) {
    std::ostringstream str;
    str << '\r';
    str << prog.progress;
    if (prog.info.total)
    {
      str << " / " << prog.info.total;
    }
    str << "      ";
    std::cout << str.str() << std::flush;
  });

  prog.startThread();
  res = ohm::load(opt.map_file.c_str(), heightmap, &load_progress, &version);
  prog.endProgress();

  std::cout << std::endl;

  if (res != 0)
  {
    std::cerr << "Failed to load heightmap. Error(" << res << "): " << ohm::errorCodeString(res) << std::endl;
    return res;
  }

  ohm::HeightmapImage hmImage(heightmap, opt.imageType(), opt.normals_mode);
  ohm::HeightmapImage::BitmapInfo info;
  hmImage.generateBitmap();
  const uint8_t *image = hmImage.bitmap(&info);
  if (!image)
  {
    std::cerr << "Failed to generate heightmap image" << std::endl;
    return 1;
  }

  std::vector<uint8_t> export_pixels;
  ExportImageType export_type = convertImage(export_pixels, image, info, opt);

  std::cout << "Saving " << opt.image_file << std::endl;
  if (!savePng(opt.image_file.c_str(), export_pixels, export_type, info.image_width, info.image_height))
  {
    std::cerr << "Failed to save heightmap image" << std::endl;
    return 1;
  }

  return res;
}