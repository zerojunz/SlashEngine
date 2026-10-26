# Makes V-HACD 4.1 decompose a mesh into the same hulls on every platform (engine/scene3d builds it
# through src/vhacd_build.cpp). Its hulls are kept in a std::map rather than an unordered_map,
# whose order each standard library sets its own way and which decides the order merge costs are
# queued in and hulls come out in; pairs whose merges cost the same are taken by their hulls' IDs,
# where each library's heap would pick its own; and the one pow call, whose last bits differ
# between C libraries, goes through VHACD_POW, which the build sets to the engine's own.
# slash_add_dependency(vhacd) runs it in V-HACD's source folder as the patch step. A script rather
# than a patch file: it works with either line ending and on a folder it already changed.

set(file include/VHACD.h)
file(READ ${file} text)
if(text MATCHES "VHACD_POW")
    return()
endif()
set(hulls "std::unordered_map<uint32_t, IVHACD::ConvexHull*>   m_hulls;")
set(order "return m_concavity > h.m_concavity ? true : false;")
set(pow "double a = std::pow(dimensions, 0.33);")
set(include "#include <unordered_set>")
foreach(check IN ITEMS "${hulls}" "${order}" "${pow}" "${include}")
    string(FIND "${text}" "${check}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${file} no longer has \"${check}\" as the patch expects")
    endif()
endforeach()
string(REPLACE "${hulls}" "std::map<uint32_t, IVHACD::ConvexHull*>   m_hulls;" text "${text}")
string(REPLACE "${order}"
    "if (m_concavity != h.m_concavity) return m_concavity > h.m_concavity; if (m_hullA != h.m_hullA) return m_hullA > h.m_hullA; return m_hullB > h.m_hullB;"
    text "${text}")
string(REPLACE "${pow}" "double a = VHACD_POW(double(dimensions), 0.33);" text "${text}")
string(REPLACE "${include}"
    "${include}\n#include <map>\n#ifndef VHACD_POW\n#define VHACD_POW std::pow\n#endif" text "${text}")
file(WRITE ${file} "${text}")
