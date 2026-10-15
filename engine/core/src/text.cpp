#include "slash/core/text.hpp"

#include <algorithm>
#include <vector>

namespace slash {

std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            const std::size_t substitute = diagonal + (a[i - 1] == b[j - 1] ? 0 : 1);
            row[j] = std::min({above + 1, row[j - 1] + 1, substitute});
            diagonal = above;
        }
    }
    return row[b.size()];
}

bool is_project_path(std::string_view path) {
    if (path.empty() || path.front() == '/' ||
        path.find_first_of("\\:") != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

} // namespace slash
