#pragma once

// The parts of asset.review that need no library: turntable views of a model, drawn headless,
// and a vision model's verdict on whether they show what was asked for.

#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace slash::library::review {

// The model turns a quarter of the way round (or a count-th) between views, seen from 20 degrees
// above, starting at its front's right: count images of size by size pixels.
struct Views {
    std::uint32_t count = 4;
    std::uint32_t size = 256;
};

// The model at a project path of root (a folder the session may read), drawn on the software
// device in front of a plain backdrop, lit by a key and a fill light that turn with the camera,
// framed by its bounds. The same pixels on every machine.
[[nodiscard]] Result<std::vector<render::Image>>
turntable(command::Session& session, const std::string& root, const std::string& path,
          const scene3d::Bounds& bounds, const Views& views);

struct Verdict {
    bool matches = false;
    // Each problem the model saw, with the view it saw it in: {"view": 2, "problem": "..."}.
    json::Array problems;
    // Problems naming a view that isn't one, left out.
    std::int64_t unverified = 0;
    std::string model;
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    bool replayed = false;
};

// Asks the model of a role in the project's providers.json ("" for the default) whether the views
// show what request asks for, given facts about the model it can't see (its size, triangles and
// checks). An error opening the role has the code ai::open gives it.
[[nodiscard]] Result<Verdict> ask(command::Session& session, const std::string& role,
                                  const std::string& request, const std::string& facts,
                                  const std::vector<render::Image>& views);

} // namespace slash::library::review
