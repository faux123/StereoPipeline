// __BEGIN_LICENSE__
// Copyright (C) 2006-2010 United States Government as represented by
// the Administrator of the National Aeronautics and Space Administration.
// All Rights Reserved.
// __END_LICENSE__


/// \file point2dem.cc
///

#ifdef _MSC_VER
#pragma warning(disable:4244)
#pragma warning(disable:4267)
#pragma warning(disable:4996)
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <stdlib.h>

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <vw/FileIO.h>
#include <vw/Image.h>
#include <vw/Math.h>
#include <vw/Cartography.h>
using namespace vw;
using namespace vw::cartography;

#include <asp/Core/OrthoRasterizer.h>
#include <asp/Core/Macros.h>

// Erases a file suffix if one exists and returns the base string
static std::string prefix_from_pointcloud_filename(std::string const& filename) {
  std::string result = filename;

  // First case: filenames that match <prefix>-PC.<suffix>
  int index = result.rfind("-PC.");
  if (index != -1) {
    result.erase(index, result.size());
    return result;
  }

  // Second case: filenames that match <prefix>.<suffix>
  index = result.rfind(".");
  if (index != -1) {
    result.erase(index, result.size());
    return result;
  }

  // No match
  return result;
}

// Apply an offset to the points in the PointImage
class PointOffsetFunc : public UnaryReturnSameType {
  Vector3 m_offset;

public:
  PointOffsetFunc(Vector3 const& offset) : m_offset(offset) {}

  template <class T>
  T operator()(T const& p) const {
    if (p == T()) return p;
    return p + m_offset;
  }
};

template <class ImageT>
UnaryPerPixelView<ImageT, PointOffsetFunc>
inline point_image_offset( ImageViewBase<ImageT> const& image, Vector3 const& offset) {
  return UnaryPerPixelView<ImageT,PointOffsetFunc>( image.impl(), PointOffsetFunc(offset) );
}

// Allows FileIO to correctly read/write these pixel types
namespace vw {
  template<> struct PixelFormatID<Vector3>   { static const PixelFormatEnum value = VW_PIXEL_GENERIC_3_CHANNEL; };
}

class PointTransFunc : public ReturnFixedType<Vector3> {
  Matrix3x3 m_trans;
public:
  PointTransFunc(Matrix3x3 trans) : m_trans(trans) {}
  Vector3 operator() (Vector3 const& pt) const { return m_trans*pt; }
};

enum ProjectionType {
  NONXYZ,
  SINUSOIDAL,
  MERCATOR,
  TRANSVERSEMERCATOR,
  ORTHOGRAPHIC,
  STEREOGRAPHIC,
  LAMBERTAZIMUTHAL,
  UTM,
  PLATECARREE
};

struct Options {
  // Input
  std::string pointcloud_filename, texture_filename;

  // Settings
  float dem_spacing, default_value;
  double semi_major, semi_minor;
  std::string reference_spheroid;
  double phi_rot, omega_rot, kappa_rot;
  std::string rot_order;
  double proj_lat, proj_lon, proj_scale;
  double x_offset, y_offset, z_offset;
  unsigned utm_zone;
  std::string cache_dir;
  ProjectionType projection;
  bool has_default_value, has_alpha, do_normalize;

  // Output
  std::string  out_prefix, output_file_type;
};

void handle_arguments( int argc, char *argv[], Options& opt ) {
  po::options_description general_options("");
  general_options.add_options()
    ("default-value", po::value(&opt.default_value), "Explicitly set the default (missing pixel) value.  By default, the min z value is used.")
    ("use-alpha", "Create images that have an alpha channel")
    ("dem-spacing,s", po::value(&opt.dem_spacing)->default_value(0.0), "Set the DEM post size (if this value is 0, the post spacing size is computed for you)")
    ("normalized,n", "Also write a normalized version of the DEM (for debugging)")
    ("orthoimage", po::value(&opt.texture_filename), "Write an orthoimage based on the texture file given as an argument to this command line option")
    ("output-prefix,o", po::value(&opt.out_prefix), "Specify the output prefix")
    ("output-filetype,t", po::value(&opt.output_file_type)->default_value("tif"), "Specify the output file")
    ("xyz-to-lonlat", "Convert from xyz coordinates to longitude, latitude, altitude coordinates.")
    ("reference-spheroid,r", po::value(&opt.reference_spheroid),"Set a reference surface to a hard coded value (one of [moon , mars].  This will override manually set datum information.")
    ("semi-major-axis", po::value(&opt.semi_major),"Set the dimensions of the datum.")
    ("semi-minor-axis", po::value(&opt.semi_minor),"Set the dimensions of the datum.")
    ("x-offset", po::value(&opt.x_offset), "Add a horizontal offset to the DEM")
    ("y-offset", po::value(&opt.y_offset), "Add a horizontal offset to the DEM")
    ("z-offset", po::value(&opt.z_offset), "Add a vertical offset to the DEM")
    ("sinusoidal", "Save using a sinusoidal projection")
    ("mercator", "Save using a Mercator projection")
    ("transverse-mercator", "Save using a transverse Mercator projection")
    ("orthographic", "Save using an orthographic projection")
    ("stereographic", "Save using a stereographic projection")
    ("lambert-azimuthal", "Save using a Lambert azimuthal projection")
    ("utm", po::value(&opt.utm_zone), "Save using a UTM projection with the given zone")
    ("proj-lat", po::value(&opt.proj_lat), "The center of projection latitude (if applicable)")
    ("proj-lon", po::value(&opt.proj_lon), "The center of projection longitude (if applicable)")
    ("proj-scale", po::value(&opt.proj_scale), "The projection scale (if applicable)")
    ("rotation-order", po::value(&opt.rot_order)->default_value("xyz"),"Set the order of an euler angle rotation applied to the 3D points prior to DEM rasterization")
    ("phi-rotation", po::value(&opt.phi_rot),"Set a rotation angle phi")
    ("omega-rotation", po::value(&opt.omega_rot),"Set a rotation angle omega")
    ("kappa-rotation", po::value(&opt.kappa_rot),"Set a rotation angle kappa")
    ("cache-dir", po::value(&opt.cache_dir)->default_value("/tmp"),"Change if can't write large files to /tmp (i.e. Super Computer)")
    ("help,h", "Display this help message");

  po::options_description positional("");
  positional.add_options()
    ("input-file", po::value(&opt.pointcloud_filename), "Input Point Cloud");

  po::positional_options_description positional_desc;
  positional_desc.add("input-file", 1);

  po::options_description all_options;
  all_options.add(general_options).add(positional);

  po::variables_map vm;
  try {
    po::store( po::command_line_parser( argc, argv ).options(all_options).positional(positional_desc).run(), vm );
    po::notify( vm );
  } catch (po::error &e) {
    vw_throw( ArgumentErr() << "Error parsing input:\n\t"
              << e.what() << general_options );
  }

  std::ostringstream usage;
  usage << "Usage: " << argv[0] << " <point-cloud> ...\n";

  if ( opt.pointcloud_filename.empty() )
    vw_throw( ArgumentErr() << "Missing point cloud.\n"
              << usage.str() << general_options );
  if ( vm.count("help") )
    vw_throw( ArgumentErr() << usage.str() << general_options );
  if ( opt.out_prefix.empty() )
    opt.out_prefix =
      prefix_from_pointcloud_filename( opt.pointcloud_filename );

  boost::to_lower( opt.reference_spheroid );
  if (!vm.count("xyz-to-lonlat"))        opt.projection = NONXYZ;
  else if ( vm.count("sinusoidal") )     opt.projection = SINUSOIDAL;
  else if ( vm.count("mercator") )       opt.projection = MERCATOR;
  else if ( vm.count("transverse-mercator") ) opt.projection = TRANSVERSEMERCATOR;
  else if ( vm.count("orthographic") )   opt.projection = ORTHOGRAPHIC;
  else if ( vm.count("stereographic") )  opt.projection = STEREOGRAPHIC;
  else if ( vm.count("lambert-azimuthal") ) opt.projection = LAMBERTAZIMUTHAL;
  else if ( vm.count("utm") )            opt.projection = UTM;
  else                                   opt.projection = PLATECARREE;
  opt.has_default_value = vm.count("default-value");
  opt.has_alpha = vm.count("use-alpha");
  opt.do_normalize = vm.count("normalized");
}

int main( int argc, char *argv[] ) {

  Options opt;
  try {
    handle_arguments( argc, argv, opt );

    DiskImageView<Vector3> point_disk_image(opt.pointcloud_filename);
    ImageViewRef<Vector3> point_image = point_disk_image;

    // Apply an (optional) rotation to the 3D points before building the mesh.
    if (opt.phi_rot != 0 || opt.omega_rot != 0 || opt.kappa_rot != 0) {
      vw_out() << "\t--> Applying rotation sequence: " << opt.rot_order
               << "      Angles: " << opt.phi_rot << "   "
               << opt.omega_rot << "  " << opt.kappa_rot << "\n";
      Matrix3x3 rotation_trans =
        math::euler_to_rotation_matrix(opt.phi_rot, opt.omega_rot,
                                       opt.kappa_rot, opt.rot_order);
      point_image =
        per_pixel_filter(point_image, PointTransFunc(rotation_trans));
    }

    if ( opt.projection != NONXYZ ) {
      // First determine if we should using a longitude range between
      // [-180, 180] or [0,360]. We determine this by looking at the
      // average location of the points. If the average location has a
      // negative x value (think in ECEF coordinates) then we should
      // be using [0,360].
      int subsample_amt = norm_2(Vector2(point_image.cols(),point_image.rows())) / 1024;
      if (subsample_amt < 1 ) subsample_amt = 1;
      Vector3 avg_location =
        mean_pixel_value(subsample(point_image, subsample_amt));

      vw_out() << "\t--> Reprojecting points into longitude, latitude, altitude.\n";
      point_image =
        cartography::xyz_to_lon_lat_radius(point_image, true,
                                           avg_location[0] >= 0);
    }

    if (opt.x_offset != 0 || opt.y_offset != 0 || opt.z_offset != 0) {
      vw_out() << "\t--> Applying offset: " << opt.x_offset
               << " " << opt.y_offset << " " << opt.z_offset << "\n";
      point_image =
        point_image_offset(point_image,
                           Vector3(opt.x_offset,opt.y_offset,opt.z_offset));
    }

    // Select a cartographic datum. There are several hard coded datums
    // that can be used here, or the user can specify their own.
    cartography::Datum datum;
    if ( opt.reference_spheroid != "" ) {
      if (opt.reference_spheroid == "mars") {
        const double MOLA_PEDR_EQUATORIAL_RADIUS = 3396190.0;
        vw_out() << "\t--> Re-referencing altitude values using standard MOLA spherical radius: " << MOLA_PEDR_EQUATORIAL_RADIUS << "\n";
        datum.set_well_known_datum("D_MARS");
      } else if (opt.reference_spheroid == "moon") {
        const double LUNAR_RADIUS = 1737400;
        vw_out() << "\t--> Re-referencing altitude values using standard lunar spherical radius: " << LUNAR_RADIUS << "\n";
        datum.set_well_known_datum("D_MOON");
      } else {
        vw_throw( ArgumentErr() << "\t--> Unknown reference spheriod: "
                  << opt.reference_spheroid
                  << ". Current options are [ moon, mars ]\nExiting." );
      }
    } else if (opt.semi_major != 0 && opt.semi_minor != 0) {
      vw_out() << "\t--> Re-referencing altitude values to user supplied datum.  Semi-major: " << opt.semi_major << "  Semi-minor: " << opt.semi_minor << "\n";
      datum = cartography::Datum("User Specified Datum",
                                 "User Specified Spheroid",
                                 "Reference Meridian",
                                 opt.semi_major, opt.semi_minor, 0.0);
    }

    // Set up the georeferencing information.  We specify everything
    // here except for the affine transform, which is defined later once
    // we know the bounds of the orthorasterizer view.  However, we can
    // still reproject the points in the point image without the affine
    // transform because this projection never requires us to convert to
    // or from pixel space.
    GeoReference georef(datum);

    // If the data was left in cartesian coordinates, we need to give
    // the DEM a projection that uses some physical units (meters),
    // rather than lon, lat.  This is actually mainly for compatibility
    // with Viz, and it's sort of a hack, but it's left in for the time
    // being.
    //
    // Otherwise, we honor the user's requested projection and convert
    // the points if necessary.
    switch( opt.projection ) {
    case NONXYZ:
      georef.set_mercator(0,0,1); break;
    case SINUSOIDAL:
      georef.set_sinusoidal(opt.proj_lon); break;
    case MERCATOR:
      georef.set_mercator(opt.proj_lat,opt.proj_lon,opt.proj_scale); break;
    case TRANSVERSEMERCATOR:
      georef.set_transverse_mercator(opt.proj_lat,opt.proj_lon,opt.proj_scale); break;
    case ORTHOGRAPHIC:
      georef.set_orthographic(opt.proj_lat,opt.proj_lon); break;
    case STEREOGRAPHIC:
      georef.set_stereographic(opt.proj_lat,opt.proj_lon,opt.proj_scale); break;
    case LAMBERTAZIMUTHAL:
      georef.set_lambert_azimuthal(opt.proj_lat,opt.proj_lon); break;
    case UTM:
      georef.set_UTM( opt.utm_zone ); break;
    default: // Handles plate carree
      break;
    }

    if ( opt.projection != NONXYZ )
      point_image = cartography::project_point_image(point_image, georef);

    // Rasterize the results to a temporary file on disk so as to speed
    // up processing in the orthorasterizer, which accesses each pixel
    // multiple times.
    DiskCacheImageView<Vector3>
      point_image_cache(point_image, "tif",
                        TerminalProgressCallback("asp","Cache: "),
                        opt.cache_dir);

    // write out the DEM, texture, and extrapolation mask as
    // georeferenced files.
    OrthoRasterizerView<PixelGray<float> >
      rasterizer(point_image_cache,
                 select_channel(point_image_cache,2),
                 opt.dem_spacing);
    if (!opt.has_default_value) {
      rasterizer.set_use_minz_as_default(true);
    } else {
      rasterizer.set_use_minz_as_default(false);
      rasterizer.set_default_value(opt.default_value);
    }

    if (opt.has_alpha)
      rasterizer.set_use_alpha(true);

    vw::BBox3 dem_bbox = rasterizer.bounding_box();
    vw_out() << "\nDEM Bounding box: " << dem_bbox << "\n";

    // Now we are ready to specify the affine transform.
    Matrix3x3 georef_affine_transform = rasterizer.geo_transform();
    vw_out() << "Georeferencing Transform: " << georef_affine_transform << "\n";
    georef.set_transform(georef_affine_transform);

    // Write out a georeferenced orthoimage of the DTM with alpha.
    if (!opt.texture_filename.empty()) {
      rasterizer.set_use_minz_as_default(false);
      DiskImageView<PixelGray<float> > texture(opt.texture_filename);
      rasterizer.set_texture(texture);
      ImageViewRef<PixelGray<float> > block_drg_raster =
        block_cache(rasterizer, Vector2i(rasterizer.cols(), 2048), 0);
      if (opt.has_alpha) {
        write_georeferenced_image(opt.out_prefix + "-DRG.tif",
                                  channel_cast_rescale<uint8>(apply_mask(block_drg_raster,PixelGray<float>(-32000))),
                                  georef, TerminalProgressCallback("asp","") );
      } else {
        write_georeferenced_image(opt.out_prefix + "-DRG.tif",
                                  channel_cast_rescale<uint8>(block_drg_raster),
                                  georef, TerminalProgressCallback("asp","") );
      }
    } else {
      { // Write out the DEM.
        vw_out() << "\nWriting DEM.\n";
        ImageViewRef<PixelGray<float> > block_dem_raster =
          block_cache(rasterizer, Vector2i(rasterizer.cols(), 128), 0);
        write_georeferenced_image( opt.out_prefix + "-DEM." +
                                   opt.output_file_type,
                                   block_dem_raster, georef,
                                   TerminalProgressCallback("asp",""));
      }

      // Write out a normalized version of the DTM (for debugging)
      if (opt.do_normalize) {
        vw_out() << "\nWriting normalized DEM.\n";

        DiskImageView<PixelGray<float> >
          dem_image(opt.out_prefix + "-DEM." + opt.output_file_type);

        write_georeferenced_image( opt.out_prefix + "-DEM-normalized.tif",
                                   channel_cast_rescale<uint8>(normalize(dem_image)),
                                   georef, TerminalProgressCallback("asp","") );
      }
    }

  } ASP_STANDARD_CATCHES;

  return 0;
}
