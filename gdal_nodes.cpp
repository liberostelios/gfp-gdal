#include "gdal_nodes.hpp"

#include <geos_c.h>

#include <unordered_map>
#include <variant>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace geoflow::nodes::gdal
{

void OGRLoaderNode::push_attributes(OGRFeature &poFeature)
{
  for (auto &[name, mterm] : poly_output("attributes").get_terminals())
  {
    auto term = (gfBasicMonoOutputTerminal *)(mterm.get());
    if (term->accepts_type(typeid(vec1i)))
    {
      //        std::cout << term->has_data() << "\n";
      auto &term_data = poly_output("attributes").get_basic<vec1i &>(name);
      term_data.push_back(poFeature.GetFieldAsInteger64(name.c_str()));
    }
    else if (term->accepts_type(typeid(vec1f)))
    {
      auto &term_data = poly_output("attributes").get_basic<vec1f &>(name);
      term_data.push_back(poFeature.GetFieldAsDouble(name.c_str()));
    }
    else if (term->accepts_type(typeid(vec1s)))
    {
      auto &term_data = poly_output("attributes").get_basic<vec1s &>(name);
      term_data.push_back((std::string)poFeature.GetFieldAsString(name.c_str()));
    }
  }
}

void OGRLoaderNode::process()
{
  GDALDatasetUniquePtr poDS(GDALDataset::Open(manager.substitute_globals(filepath).c_str(), GDAL_OF_VECTOR));
  if (poDS == nullptr)
  {
    std::cerr << "Open failed.\n";
    return;
  }
  layer_count = poDS->GetLayerCount();
  std::cout << "Layer count: " << layer_count << "\n";
  layer_id = 0;

  // Set up vertex data (and buffer(s)) and attribute pointers
  // LineStringCollection line_strings;
  // LinearRingCollection linear_rings;
  auto &linear_rings = vector_output("linear_rings");
  auto &line_strings = vector_output("line_strings");

  OGRLayer *poLayer;
  poLayer = poDS->GetLayer(layer_id);
  std::cout << "Layer " << layer_id << " feature count: " << poLayer->GetFeatureCount() << "\n";
  geometry_type = poLayer->GetGeomType();
  geometry_type_name = OGRGeometryTypeToName(geometry_type);
  std::cout << "Layer geometry type: " << geometry_type_name << "\n";

  auto layer_def = poLayer->GetLayerDefn();
  auto field_count = layer_def->GetFieldCount();

  for (size_t i = 0; i < field_count; ++i)
  {
    auto field_def = layer_def->GetFieldDefn(i);
    auto t = field_def->GetType();
    auto field_name = (std::string)field_def->GetNameRef();
    if (t == OFTInteger || t == OFTInteger64)
    {
      auto &term = poly_output("attributes").add(field_name, typeid(vec1i));
      term.set(vec1i());
    }
    else if (t == OFTString)
    {
      auto &term = poly_output("attributes").add(field_name, typeid(vec1s));
      term.set(vec1s());
    }
    else if (t == OFTReal)
    {
      auto &term = poly_output("attributes").add(field_name, typeid(vec1f));
      term.set(vec1f());
    }
  }

  bool found_offset = manager.data_offset.has_value();
  poLayer->ResetReading();

  for (auto &poFeature : poLayer)
  {
    // read feature geometry
    OGRGeometry *poGeometry;
    poGeometry = poFeature->GetGeometryRef();
    // std::cout << "Layer geometry type: " << poGeometry->getGeometryType() << " , " << geometry_type << "\n";
    if (poGeometry != nullptr) // FIXME: we should check if te layer geometrytype matches with this feature's geometry type. Messy because they can be a bit different eg. wkbLineStringZM and wkbLineString25D
    {

      if (
          poGeometry->getGeometryType() == wkbLineString25D || poGeometry->getGeometryType() == wkbLineStringZM ||
          poGeometry->getGeometryType() == wkbLineString)
      {
        OGRLineString *poLineString = poGeometry->toLineString();

        LineString line_string;
        for (auto &poPoint : poLineString)
        {
          if (!found_offset)
          {
            manager.data_offset = {poPoint.getX(), poPoint.getY(), 0};
            found_offset = true;
          }
          std::array<float, 3> p = {
              float(poPoint.getX() - (*manager.data_offset)[0]),
              float(poPoint.getY() - (*manager.data_offset)[1]),
              float(poPoint.getZ() - (*manager.data_offset)[2]) + base_elevation};
          line_string.push_back(p);
        }
        line_strings.push_back(line_string);

        push_attributes(*poFeature);
      }
      else if (poGeometry->getGeometryType() == wkbPolygon25D || poGeometry->getGeometryType() == wkbPolygon || poGeometry->getGeometryType() == wkbPolygonZM || poGeometry->getGeometryType() == wkbPolygonM)
      {
        OGRPolygon *poPolygon = poGeometry->toPolygon();

        LinearRing gf_polygon;
        // for(auto& poPoint : poPolygon->getExteriorRing()) {
        OGRPoint poPoint;
        for (size_t i = 0; i < poPolygon->getExteriorRing()->getNumPoints() - 1; ++i)
        {
          poPolygon->getExteriorRing()->getPoint(i, &poPoint);
          if (!found_offset)
          {
            manager.data_offset = {poPoint.getX(), poPoint.getY(), 0};
            found_offset = true;
          }
          std::array<float, 3> p = {float(poPoint.getX() - (*manager.data_offset)[0]), float(poPoint.getY() - (*manager.data_offset)[1]), float(poPoint.getZ() - (*manager.data_offset)[2]) + base_elevation};
          gf_polygon.push_back(p);
        }
        // also read the interior rings (holes)
        for (size_t i = 0; i < poPolygon->getNumInteriorRings(); ++i) 
        {
          auto ogr_iring = poPolygon->getInteriorRing(i);
          vec3f gf_iring;
          for (size_t j = 0; j < ogr_iring->getNumPoints() - 1; ++j)
          {
            ogr_iring->getPoint(j, &poPoint);
            std::array<float, 3> p = {float(poPoint.getX() - (*manager.data_offset)[0]), float(poPoint.getY() - (*manager.data_offset)[1]), float(poPoint.getZ() - (*manager.data_offset)[2]) + base_elevation};
            gf_iring.push_back(p);
          }
          gf_polygon.interior_rings().push_back(gf_iring);
        }
        // ring.erase(--ring.end());
        // bg::model::polygon<point_type_3d> boost_poly;
        // for (auto& p : ring) {
        //   bg::append(boost_poly.outer(), point_type_3d(p[0], p[1], p[2]));
        // }
        // bg::unique(boost_poly);
        // vec3f ring_dedup;
        // for (auto& p : boost_poly.outer()) {
        //   ring_dedup.push_back({float(bg::get<0>(p)), float(bg::get<1>(p)), float(bg::get<2>(p))}); //FIXME losing potential z...
        // }
        linear_rings.push_back(gf_polygon);

        push_attributes(*poFeature);
      }
      else
      {
        std::cout << "unsupported geometry\n";
      }
    }
  }
  // if (geometry_type == wkbLineString25D || geometry_type == wkbLineStringZM) {
  if (line_strings.size() > 0)
  {
    // output("line_strings").set(line_strings);
    std::cout << "pushed " << line_strings.size() << " line_string features...\n";
    // } else if (geometry_type == wkbPolygon || geometry_type == wkbPolygon25D || geometry_type == wkbPolygonZM || geometry_type == wkbPolygonM) {
  }
  else if (linear_rings.size() > 0)
  {
    // output("linear_rings").set(linear_rings);
    std::cout << "pushed " << linear_rings.size() << " linear_ring features...\n";
  }

  //    for(auto& [name, term] : poly_output("attributes").terminals) {
  //      std::cout << "group_term " << name << "\n";
  //      if (term->type == typeid(vec1f))
  //        for (auto& val : term->get<vec1f>()) {
  //          std::cout << "\t" << val << "\n";
  //        }
  //      if (term->type == typeid(vec1i))
  //        for (auto& val : term->get<vec1i>()) {
  //          std::cout << "\t" << val << "\n";
  //        }
  //      if (term->type == typeid(vec1s))
  //        for (auto& val : term->get<vec1s>()) {
  //          std::cout << "\t" << val << "\n";
  //        }
  //    }
}

void OGRWriterNode::process()
{
  auto& geom_term = vector_input("geometries");

  //    const char *gszDriverName = "ESRI Shapefile";
  GDALDriver* poDriver;

  GDALAllRegister();

  poDriver = GetGDALDriverManager()->GetDriverByName(gdaldriver.c_str());
  if (poDriver == nullptr) {
    printf("%s driver not available.\n", gdaldriver.c_str());
    exit(1);
  }

  GDALDataset* poDS;

  // TODO: The driver and layer creation options (papszOptions) seem to have no
  //  effect at all. I'm not sure what to do

  // For parsing GDAL KEY=VALUE options, see the CSL* functions in https://gdal.org/api/cpl.html#cpl-string-h

  // Driver creation options. For now there is only one option possible.
  //  char** papszOptions = (char**)CPLCalloc(sizeof(char*), 2);
  char** papszOptions = nullptr;
  if (append) {
    papszOptions =
      CSLSetNameValue(papszOptions, "APPEND_SUBDATASET", "YES");
  } else {
    papszOptions =
      CSLSetNameValue(papszOptions, "APPEND_SUBDATASET", "NO");
  }
  std::string bla("APPEND_SUBDATASET=YES");
  CPLParseNameValue(bla.c_str(), nullptr);
  std::cout << std::endl  << "APPEND_SUBDATASET=" << CSLFetchNameValue(papszOptions, "APPEND_SUBDATASET") << std::endl;
  // Create the driver
  poDS = poDriver->Create(
    manager.substitute_globals(filepath).c_str(), 0, 0, 0, GDT_Unknown, papszOptions);
  if (poDS == nullptr) {
    printf("Creation of output file failed.\n");
    exit(1);
  }
  CSLDestroy(papszOptions);

  OGRSpatialReference oSRS;
  OGRLayer*           poLayer;

  oSRS.importFromEPSG(epsg);
  OGRwkbGeometryType wkbType;
  if (geom_term.is_connected_type(typeid(LinearRing))) {
    wkbType = wkbPolygon;
  } else if (geom_term.is_connected_type(typeid(LineString))) {
    wkbType = wkbLineString25D;
  } else if (geom_term.is_connected_type(typeid(TriangleCollection))) {
    wkbType = wkbPolyhedralSurfaceZ;
  }

//  // Parse Layer Creation Options
//  std::vector<std::string> lco_vec;
//  std::stringstream        s_stream(lco);
//  while (s_stream.good()) {
//    std::string substr;
//    getline(s_stream, substr, ',');
//    lco_vec.push_back(substr);
//  }
//  char** papszOptionsLayer = nullptr;
//  papszOptionsLayer =
//    CSLSetNameValue(papszOptionsLayer, "APPEND_SUBDATASET", "YES");
//  for (auto & i : lco_vec) {
////    papszOptionsLayer[i] = const_cast<char*>(lco_vec[i].c_str());
//  }

  // Frickin CreateLayer takes a char** for the lco. Whats a char** anyways!?
  poLayer =
    poDS->CreateLayer(layername.c_str(), &oSRS, wkbType, nullptr);
  if (poLayer == nullptr) {
    printf("Layer creation failed.\n");
    exit(1);
  }

  std::unordered_map<std::string, size_t> attr_id_map;
  int fcnt = poLayer->GetLayerDefn()->GetFieldCount();
  for (auto& term : poly_input("attributes").basic_terminals()) {
    //      std::cout << "group_term " << name << "\n";
    auto name = term->get_name();
    if (term->accepts_type(typeid(vec1f))) {
      OGRFieldDefn oField(name.c_str(), OFTReal);
      if (poLayer->CreateField(&oField) != OGRERR_NONE) {
        printf("Creating Name field failed.\n");
        exit(1);
      }
      attr_id_map[name] = fcnt++;
    } else if (term->accepts_type(typeid(vec1i))) {
      OGRFieldDefn oField(name.c_str(), OFTInteger64);
      if (poLayer->CreateField(&oField) != OGRERR_NONE) {
        printf("Creating Name field failed.\n");
        exit(1);
      }
      attr_id_map[name] = fcnt++;
    } else if (term->accepts_type(typeid(vec1s))) {
      OGRFieldDefn oField(name.c_str(), OFTString);
      if (poLayer->CreateField(&oField) != OGRERR_NONE) {
        printf("Creating Name field failed.\n");
        exit(1);
      }
      attr_id_map[name] = fcnt++;
    }
  }

  int l = geom_term.size();
  std::cout << "input geometries length " << l << std::endl;
  for (size_t i = 0; i != geom_term.size(); ++i) {
    OGRFeature* poFeature;
    poFeature = OGRFeature::CreateFeature(poLayer->GetLayerDefn());
    for (auto& term : poly_input("attributes").basic_terminals()) {
      auto tname = term->get_name();
      if (term->accepts_type(typeid(vec1f))) {
        auto& val = term->get<const vec1f&>();
        poFeature->SetField(attr_id_map[tname], val[i]);
      } else if (term->accepts_type(typeid(vec1i))) {
        auto& val = term->get<const vec1i&>();
        poFeature->SetField(attr_id_map[tname], val[i]);
      } else if (term->accepts_type(typeid(vec1s))) {
        auto& val = term->get<const vec1s&>();
        poFeature->SetField(attr_id_map[tname], val[i].c_str());
      }
    }

    if (geom_term.is_connected_type(typeid(LinearRing))) {
      OGRLinearRing     ogrring;
      const LinearRing& lr = geom_term.get<LinearRing>(i);
      // set exterior ring
      for (auto& g : lr) {
        ogrring.addPoint(g[0] + (*manager.data_offset)[0],
                         g[1] + (*manager.data_offset)[1],
                         g[2] + (*manager.data_offset)[2]);
      }
      ogrring.closeRings();
      OGRPolygon ogrpoly;
      ogrpoly.addRing(&ogrring);

      // set interior rings
      for (auto& iring : lr.interior_rings())
      {
        OGRLinearRing ogr_iring;
        for (auto& g : iring)
        {
          ogr_iring.addPoint(g[0] + (*manager.data_offset)[0], g[1] + (*manager.data_offset)[1], g[2] + (*manager.data_offset)[2]);
        }
        ogr_iring.closeRings();
        ogrpoly.addRing(&ogr_iring);
      }
      poFeature->SetGeometry(&ogrpoly);
    }
    if (geom_term.is_connected_type(typeid(LineString))) {
      OGRLineString     ogrlinestring;
      const LineString& ls = geom_term.get<LineString>(i);
      for (auto& g : ls) {
        ogrlinestring.addPoint(g[0] + (*manager.data_offset)[0],
                               g[1] + (*manager.data_offset)[1],
                               g[2] + (*manager.data_offset)[2]);
      }
      poFeature->SetGeometry(&ogrlinestring);
    }
    if (geom_term.is_connected_type(typeid(TriangleCollection))) {
      OGRPolyhedralSurface ogrpolyhsrf;
      for (auto &triangle : geom_term.get<TriangleCollection>(i))
      {
        OGRPolygon ogrpoly;
        OGRLinearRing ring;
        for (auto &vertex : triangle)
        {
          ring.addPoint(vertex[0] + (*manager.data_offset)[0],
                        vertex[1] + (*manager.data_offset)[1],
                        vertex[2] + (*manager.data_offset)[2]);
        }
        ring.closeRings();
        ogrpoly.addRing(&ring);
        if (ogrpolyhsrf.addGeometryDirectly(&ogrpoly) != OGRERR_NONE) {
          printf("couldn't add triangle to polyhedralsurface");
        }
      }
      int numgeom = ogrpolyhsrf.getNumGeometries();
      int polyvalid = ogrpolyhsrf.IsValid();
      std::cout << &"num tri in polyhsrf " [ numgeom] << std::endl;
      poFeature->SetGeometry(&ogrpolyhsrf);
    }

    if (poLayer->CreateFeature(poFeature) != OGRERR_NONE) {
      printf("Failed to create feature in geopackage.\n");
      exit(1);
    }
    OGRFeature::DestroyFeature(poFeature);
  }
  GDALClose(poDS);
}

void CSVLoaderNode::process()
{
  PointCollection points;

  std::ifstream f_in(manager.substitute_globals(filepath));
  float px, py, pz;
  size_t i = 0;
  std::string header;
  std::getline(f_in, header);
  while (f_in >> px >> py >> pz)
  {
    if (i++ % thin_nth == 0)
    {
      points.push_back({px, py, pz});
    }
  }
  f_in.close();

  output("points").set(points);
}

void CSVWriterNode::process()
{
  auto points = input("points").get<PointCollection>();
  auto distances = input("distances").get<vec1f>();

  std::ofstream f_out(manager.substitute_globals(filepath));
  f_out << std::fixed << std::setprecision(2);
  f_out << "x y z distance\n";
  for (size_t i = 0; i < points.size(); ++i)
  {
    f_out
        << points[i][0] + (*manager.data_offset)[0] << " "
        << points[i][1] + (*manager.data_offset)[1] << " "
        << points[i][2] + (*manager.data_offset)[2] << " "
        << distances[i] << "\n";
  }
  f_out.close();
}

void GEOSMergeLinesNode::process()
{
  std::cout << "Merging lines\n";
  auto lines = input("lines").get<LineStringCollection>();
  GEOSContextHandle_t gc = GEOS_init_r();
  std::vector<GEOSGeometry *> linearray;
  for (int i = 0; i < lines.size(); i++)
  {
    GEOSCoordSequence *points = GEOSCoordSeq_create_r(gc, 2, 3);
    for (int j = 0; j < 2; j++)
    {
      GEOSCoordSeq_setX_r(gc, points, j, lines[i][j][0]);
      GEOSCoordSeq_setY_r(gc, points, j, lines[i][j][1]);
      GEOSCoordSeq_setZ_r(gc, points, j, lines[i][j][2]);
    }
    GEOSGeometry *line = GEOSGeom_createLineString_r(gc, points);
    linearray.push_back(line);
  }
  GEOSGeometry *lineCollection = GEOSGeom_createCollection_r(gc, GEOS_GEOMETRYCOLLECTION, linearray.data(), lines.size());
  GEOSGeometry *mergedlines = GEOSLineMerge_r(gc, lineCollection);

  LineStringCollection outputLines;
  for (int i = 0; i < GEOSGetNumGeometries_r(gc, mergedlines); i++)
  {
    const GEOSCoordSequence *l = GEOSGeom_getCoordSeq_r(gc, GEOSGetGeometryN_r(gc, mergedlines, i));
    vec3f line_vec3f;
    unsigned int size;
    GEOSCoordSeq_getSize_r(gc, l, &size);
    for (int j = 0; j < size; j++)
    {
      double x, y, z = 0;
      GEOSCoordSeq_getX_r(gc, l, j, &x);
      GEOSCoordSeq_getY_r(gc, l, j, &y);
      GEOSCoordSeq_getZ_r(gc, l, j, &z);
      line_vec3f.push_back({float(x), float(y), float(z)});
    }
    outputLines.push_back(line_vec3f);
  }

  // clean GEOS geometries
  for (auto l : linearray)
  {
    GEOSGeom_destroy_r(gc, l);
  }
  //GEOSGeom_destroy_r(gc, lineCollection);
  GEOSGeom_destroy_r(gc, mergedlines);
  GEOS_finish_r(gc);

  output("lines").set(outputLines);
}

void PolygonUnionNode::process()
{
}

} // namespace geoflow::nodes::gdal
