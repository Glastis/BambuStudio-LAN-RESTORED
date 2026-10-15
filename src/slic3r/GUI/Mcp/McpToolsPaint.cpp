// MCP tools: painting of seams, supports, fuzzy skin and colours on object surfaces.
//
// The painting gizmos only work with the mouse; here the same TriangleSelector
// machinery is driven with geometric regions given in world coordinates.

#include "McpToolsCommon.hpp"

#include <queue>

#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

static FacetsAnnotation &annotation(ModelVolume &mv, const std::string &kind)
{
    if (kind == "seam") return mv.seam_facets;
    if (kind == "support") return mv.supported_facets;
    if (kind == "color") return mv.mmu_segmentation_facets;
    if (kind == "fuzzy_skin") return mv.fuzzy_skin_facets;
    throw ToolError("`kind` must be seam, support, color or fuzzy_skin.");
}

static EnforcerBlockerType paint_state(const json &args, const std::string &kind)
{
    const std::string state = arg_str(args, "state", kind == "color" ? "filament" : "enforce");
    if (state == "clear") return EnforcerBlockerType::NONE;
    if (kind == "color") {
        const int slot = arg_int(args, "filament");
        check_filament_slot(slot);
        return EnforcerBlockerType(slot);
    }
    if (kind == "fuzzy_skin") return EnforcerBlockerType::FUZZY_SKIN;
    if (state == "enforce") return EnforcerBlockerType::ENFORCER;
    if (state == "block") return EnforcerBlockerType::BLOCKER;
    throw ToolError("`state` must be enforce, block or clear.");
}

static double dist_point_segment(const Vec3d &p, const Vec3d &a, const Vec3d &b)
{
    const Vec3d  ab = b - a;
    const double l2 = ab.squaredNorm();
    const double t  = l2 > 0 ? std::clamp((p - a).dot(ab) / l2, 0.0, 1.0) : 0.0;
    return (a + t * ab - p).norm();
}

struct Region
{
    std::string type; // all | sphere | line | height | box | facing
    Vec3d       a = Vec3d::Zero(), b = Vec3d::Zero();
    double      radius = 0;
    Vec3d       dir = Vec3d::UnitZ();
    double      max_angle_cos = 1;
    bool        has_box = false;
    Vec3d       box_min = Vec3d::Zero(), box_max = Vec3d::Zero();
};

static Region parse_region(const json &args)
{
    if (!args.contains("region") || !args["region"].is_object())
        throw ToolError("`region` is required.");
    const json &r = args["region"];
    Region      out;
    out.type = arg_str(r, "type");
    if (out.type == "sphere") {
        out.a      = arg_vec3(r, "center", Vec3d::Zero());
        out.radius = arg_num(r, "radius", 2.0);
    } else if (out.type == "line") {
        out.a      = arg_vec3(r, "start", Vec3d::Zero());
        out.b      = arg_vec3(r, "end", Vec3d::Zero());
        out.radius = arg_num(r, "radius", 1.0);
    } else if (out.type == "facing") {
        out.dir = arg_vec3(r, "direction", Vec3d::UnitZ());
        if (out.dir.norm() < EPSILON) throw ToolError("`direction` must not be zero.");
        out.dir.normalize();
        out.max_angle_cos = std::cos(arg_num(r, "max_angle", 30.0) * M_PI / 180.0);
    } else if (out.type == "height") {
        out.a = Vec3d(0, 0, arg_num(r, "z_min", 0.0));
        out.b = Vec3d(0, 0, arg_num(r, "z_max", 1e9));
        if (out.b.z() <= out.a.z()) throw ToolError("`z_max` must be greater than `z_min`.");
    } else if (out.type != "box" && out.type != "all") {
        throw ToolError("`region.type` must be all, sphere, line, height, box or facing.");
    }
    if (has_arg(r, "min") || has_arg(r, "max") || out.type == "box") {
        out.has_box = true;
        out.box_min = arg_vec3(r, "min", Vec3d::Constant(-1e9));
        out.box_max = arg_vec3(r, "max", Vec3d::Constant(1e9));
    }
    if ((out.type == "sphere" || out.type == "line") && out.radius <= 0)
        throw ToolError("`radius` must be > 0.");
    return out;
}

// Paints one volume. Returns the number of original triangles touched.
static size_t paint_volume(ModelVolume &mv, const Transform3d &instance_trafo, FacetsAnnotation &facets, const Region &region,
                           EnforcerBlockerType state)
{
    const TriangleMesh        &mesh  = mv.mesh();
    const indexed_triangle_set &its  = mesh.its;
    const Transform3d          trafo = instance_trafo * mv.get_matrix();
    Transform3d                trafo_no_translate = trafo;
    trafo_no_translate.translation()              = Vec3d::Zero();

    TriangleSelector selector(mesh);
    selector.deserialize(facets.get_data());

    std::vector<char> candidate(its.indices.size(), 0);
    size_t            count = 0;
    for (size_t f = 0; f < its.indices.size(); ++f) {
        Vec3d v[3];
        for (int k = 0; k < 3; ++k) v[k] = trafo * its.vertices[its.indices[f][k]].cast<double>();
        const Vec3d centroid = (v[0] + v[1] + v[2]) / 3.0;
        if (region.has_box) {
            auto inside = [&region](const Vec3d &p) {
                return (p.array() >= region.box_min.array()).all() && (p.array() <= region.box_max.array()).all();
            };
            if (!inside(centroid) && !inside(v[0]) && !inside(v[1]) && !inside(v[2])) continue;
        }

        bool hit = false;
        if (region.type == "all" || region.type == "box") hit = true;
        else if (region.type == "height") {
            const double lo = std::min({v[0].z(), v[1].z(), v[2].z()});
            const double hi = std::max({v[0].z(), v[1].z(), v[2].z()});
            hit             = hi >= region.a.z() && lo <= region.b.z();
        }
        else if (region.type == "facing") {
            // Vertices are already in world space (mirroring keeps the winding consistent with the outward normal).
            Vec3d n = (v[1] - v[0]).cross(v[2] - v[0]);
            if (trafo.linear().determinant() < 0) n = -n;
            hit = n.norm() > 0 && n.normalized().dot(region.dir) >= region.max_angle_cos;
        } else {
            // Sample vertices, edge midpoints and centroid.
            const Vec3d samples[7] = {v[0], v[1], v[2], (v[0] + v[1]) / 2, (v[1] + v[2]) / 2, (v[2] + v[0]) / 2, centroid};
            for (const Vec3d &s : samples) {
                const double d = region.type == "sphere" ? (s - region.a).norm() : dist_point_segment(s, region.a, region.b);
                if (d <= region.radius) { hit = true; break; }
            }
        }
        if (!hit) continue;
        candidate[f] = 1;
        ++count;
    }

    if (region.type == "sphere" || region.type == "line" || region.type == "height") {
        // Brush-like painting with triangle splitting: one select_patch() per connected group of candidates.
        const Transform3d inv    = trafo.inverse();
        const Vec3f       a_mesh = (inv * region.a).cast<float>();
        const Vec3f       b_mesh = (inv * region.b).cast<float>();
        const Vec3f       camera = (inv * (region.a + Vec3d(0, 0, 1000))).cast<float>();
        const auto        neighbors = its_face_neighbors(its);
        std::vector<char> visited(its.indices.size(), 0);
        for (size_t f = 0; f < its.indices.size(); ++f) {
            if (!candidate[f] || visited[f]) continue;
            std::queue<int> q;
            q.push(int(f));
            visited[f] = 1;
            while (!q.empty()) {
                const int cur = q.front();
                q.pop();
                for (int k = 0; k < 3; ++k) {
                    const int n = neighbors[cur][k];
                    if (n >= 0 && candidate[n] && !visited[n]) { visited[n] = 1; q.push(n); }
                }
            }
            std::unique_ptr<TriangleSelector::Cursor> cursor =
                region.type == "height" ?
                    TriangleSelector::SinglePointCursor::cursor_factory(float(region.a.z()), camera, float(region.b.z() - region.a.z()), trafo,
                                                                        TriangleSelector::ClippingPlane()) :
                region.type == "sphere" ?
                    TriangleSelector::SinglePointCursor::cursor_factory(a_mesh, camera, float(region.radius), TriangleSelector::SPHERE, trafo,
                                                                        TriangleSelector::ClippingPlane()) :
                    TriangleSelector::DoublePointCursor::cursor_factory(a_mesh, b_mesh, camera, float(region.radius), TriangleSelector::SPHERE, trafo,
                                                                        TriangleSelector::ClippingPlane());
            selector.select_patch(int(f), std::move(cursor), state, trafo_no_translate, true);
        }
    } else {
        for (size_t f = 0; f < its.indices.size(); ++f)
            if (candidate[f]) selector.set_facet(int(f), state);
    }

    if (count > 0) facets.set(selector);
    return count;
}

static void close_painting_gizmos()
{
    // An open painting gizmo keeps its own copy of the painting and would overwrite ours.
    GLGizmosManager &gizmos = plater().canvas3D()->get_gizmos_manager();
    if (gizmos.get_current_type() != GLGizmosManager::Undefined) gizmos.reset_all_states();
}

static void after_paint(int obj_idx)
{
    if (ObjectList *list = wxGetApp().obj_list()) list->update_info_items(size_t(obj_idx));
    plater().canvas3D()->reload_scene(true);
    plater().schedule_background_process();
    plater().canvas3D()->set_as_dirty();
}

void register_paint_tools(ToolRegistry &r)
{
    r.add({"paint_facets",
           "Paint the surface of an object like the painting tools of Bambu Studio: seam enforcers/blockers, support "
           "enforcers/blockers, fuzzy skin or colours (multi-material painting). Regions use WORLD coordinates of the given "
           "instance (see list_objects bounding boxes):\n"
           "- {type:'line', start, end, radius}: a brush stroke, e.g. a vertical seam line along the back of the model;\n"
           "- {type:'sphere', center, radius}: a round brush;\n"
           "- {type:'height', z_min, z_max}: a horizontal band, cut precisely (e.g. colour the top 5 mm);\n"
           "- {type:'facing', direction, max_angle}: every facet whose normal is within max_angle degrees of direction "
           "(e.g. direction {x:0,y:0,z:-1} = overhangs, {x:0,y:1,z:0} = back faces);\n"
           "- {type:'box', min, max}: every facet touching the box (whole triangles, not cut);\n"
           "- {type:'all'}.\n"
           "Any region may also carry `min`/`max` to restrict it to a box. state 'clear' erases. Use paint_info to check the result.",
           schema({{"object", prop_int("Object index.")},
                   {"instance", prop_int("Instance whose position defines the world coordinates (default 0).")},
                   {"volume", prop_int("Only paint this part (default: every model part of the object).")},
                   {"kind", prop_enum("What to paint.", {"seam", "support", "color", "fuzzy_skin"})},
                   {"state", prop_enum("enforce/block for seam and support, clear to erase (colour uses `filament`).", {"enforce", "block", "clear"})},
                   {"filament", prop_int("1-based filament slot, for kind=color.")},
                   {"region", json{{"type", "object"}, {"description", "Region to paint, see the tool description."}}}},
                  {"object", "kind", "region"}),
           true, false, false, [](const json &args) {
               const int         obj   = arg_int(args, "object");
               const std::string kind  = arg_str(args, "kind");
               ModelObject      &mo    = object_at(obj);
               const Transform3d inst  = instance_at(obj, arg_int(args, "instance", 0)).get_matrix();
               const Region      reg   = parse_region(args);
               const auto        state = paint_state(args, kind);

               std::vector<int> volumes;
               if (has_arg(args, "volume")) {
                   volume_at(obj, arg_int(args, "volume"));
                   volumes.push_back(arg_int(args, "volume"));
               } else
                   for (int v = 0; v < int(mo.volumes.size()); ++v)
                       if (mo.volumes[v]->is_model_part()) volumes.push_back(v);

               close_painting_gizmos();
               Plater::TakeSnapshot snapshot(&plater(), "MCP: paint " + kind);
               json   touched = json::object();
               size_t total   = 0;
               for (int v : volumes) {
                   ModelVolume &mv = *mo.volumes[v];
                   const size_t n  = paint_volume(mv, inst, annotation(mv, kind), reg, state);
                   touched[std::to_string(v)] = n;
                   total += n;
               }
               if (total == 0)
                   throw ToolError("The region does not touch the object. Check the coordinates against list_objects bounding boxes.");
               after_paint(obj);
               return ToolResult::data({{"object", obj}, {"kind", kind}, {"triangles_touched_per_volume", touched}});
           }});

    r.add({"clear_painting", "Remove all painting of a kind (or every kind) from an object.",
           schema({{"object", prop_int("Object index.")}, {"kind", prop_enum("Kind to clear (default: all).", {"seam", "support", "color", "fuzzy_skin", "all"})}},
                  {"object"}),
           true, false, true, [](const json &args) {
               const int         obj  = arg_int(args, "object");
               const std::string kind = arg_str(args, "kind", "all");
               ModelObject      &mo   = object_at(obj);
               close_painting_gizmos();
               Plater::TakeSnapshot snapshot(&plater(), "MCP: clear painting");
               for (ModelVolume *mv : mo.volumes) {
                   for (const char *k : {"seam", "support", "color", "fuzzy_skin"})
                       if (kind == "all" || kind == k) annotation(*mv, k).reset();
               }
               after_paint(obj);
               return ToolResult::data(object_summary_json(obj, true));
           }});

    r.add({"paint_info", "Count the painted triangles of each kind and state on an object.", schema({{"object", prop_int("Object index.")}}, {"object"}),
           true, true, false, [](const json &args) {
               const int    obj = arg_int(args, "object");
               ModelObject &mo  = object_at(obj);
               json         out = json::array();
               for (int v = 0; v < int(mo.volumes.size()); ++v) {
                   ModelVolume &mv = *mo.volumes[v];
                   json         vo = {{"volume", v}, {"name", mv.name}};
                   for (const char *k : {"seam", "support", "fuzzy_skin"}) {
                       TriangleSelector ts(mv.mesh());
                       ts.deserialize(annotation(mv, k).get_data());
                       vo[k] = {{"enforced", ts.num_facets(EnforcerBlockerType::ENFORCER)}, {"blocked", ts.num_facets(EnforcerBlockerType::BLOCKER)}};
                   }
                   TriangleSelector ts(mv.mesh());
                   ts.deserialize(mv.mmu_segmentation_facets.get_data());
                   json colors = json::object();
                   for (int slot = 1; slot <= int(EnforcerBlockerType::ExtruderMax); ++slot)
                       if (int n = ts.num_facets(EnforcerBlockerType(slot)); n > 0) colors[std::to_string(slot)] = n;
                   vo["color_triangles_per_filament"] = colors;
                   out.push_back(vo);
               }
               return ToolResult::data(out);
           }});
}

}}} // namespace Slic3r::GUI::Mcp
