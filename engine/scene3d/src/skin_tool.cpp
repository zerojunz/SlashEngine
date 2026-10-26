#include "slash/scene3d/skin_tool.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

constexpr int kNone = -1;

// Godot's DisjointSet (core/templates/disjoint_set.h): union by rank, its elements and sets
// listed in the order they were first inserted, as its insertion-ordered HashMap lists them.
class DisjointSet {
public:
    void insert(std::size_t x) {
        if (elements_.try_emplace(x, Element{.parent = x}).second) {
            order_.push_back(x);
        }
    }

    void create_union(std::size_t a, std::size_t b) {
        insert(a);
        insert(b);
        std::size_t x = root(a);
        std::size_t y = root(b);
        if (x == y) {
            return;
        }
        if (elements_.at(x).rank < elements_.at(y).rank) {
            std::swap(x, y);
        }
        elements_.at(y).parent = x;
        if (elements_.at(x).rank == elements_.at(y).rank) {
            ++elements_.at(x).rank;
        }
    }

    [[nodiscard]] std::vector<std::size_t> representatives() {
        std::vector<std::size_t> out;
        for (const std::size_t x : order_) {
            if (elements_.at(x).parent == x) {
                out.push_back(x);
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<std::size_t> members(std::size_t representative) {
        std::vector<std::size_t> out;
        for (const std::size_t x : order_) {
            if (root(x) == representative) {
                out.push_back(x);
            }
        }
        return out;
    }

private:
    struct Element {
        std::size_t parent = 0;
        int rank = 0;
    };

    std::size_t root(std::size_t x) {
        std::size_t r = x;
        while (elements_.at(r).parent != r) {
            r = elements_.at(r).parent;
        }
        while (elements_.at(x).parent != r) {
            x = std::exchange(elements_.at(x).parent, r);
        }
        return r;
    }

    std::map<std::size_t, Element> elements_;
    std::vector<std::size_t> order_;
};

bool has(const std::vector<std::size_t>& list, std::size_t x) {
    return std::ranges::find(list, x) != list.end();
}

// A node as SkinTool sees it, a GLTFNode.
struct Node {
    int parent = kNone;
    std::vector<std::size_t> children;
    int height = 0;
    int key = 0;        // its index in the file, which Godot sorts by
    bool joint = false; // a joint of some skin
    bool mesh = false;
    bool skinned = false; // a mesh a skin deforms
    int skeleton = kNone;
};

struct Skin {
    std::vector<std::size_t> joints;
    std::vector<std::size_t> non_joints;
    std::vector<std::size_t> roots;
};

class Tool {
public:
    explicit Tool(const Model& model) : nodes_(model.nodes.size()) {
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const ModelNode& in = model.nodes[i];
            Node& node = nodes_[i];
            node.parent = in.parent;
            node.key = in.file_index >= 0 ? in.file_index : static_cast<int>(i);
            if (in.parent >= 0) {
                nodes_[static_cast<std::size_t>(in.parent)].children.push_back(i);
                node.height = nodes_[static_cast<std::size_t>(in.parent)].height + 1;
            }
        }
        for (const ModelPart& part : model.parts) {
            if (part.node_index >= 0) {
                Node& node = nodes_[static_cast<std::size_t>(part.node_index)];
                node.mesh = true;
                node.skinned = node.skinned || part.skin >= 0;
            }
        }
        for (const ModelSkin& in : model.skins) {
            Skin skin;
            skin.joints = in.joints;
            for (const std::size_t joint : in.joints) {
                nodes_[joint].joint = true;
            }
            skins_.push_back(std::move(skin));
        }
    }

    // SkinTool::_expand_skin, _determine_skeletons and the bone order of _create_skeletons.
    std::vector<std::vector<std::size_t>> skeletons() {
        for (Skin& skin : skins_) {
            expand_skin(skin);
        }
        const std::vector<std::vector<std::size_t>> joints = determine_skeletons();
        std::vector<std::vector<std::size_t>> out;
        for (std::size_t skeleton = 0; skeleton < joints.size(); ++skeleton) {
            for (const std::size_t joint : joints[skeleton]) {
                if (nodes_[joint].skeleton == kNone) {
                    nodes_[joint].skeleton = static_cast<int>(skeleton);
                }
            }
        }
        for (std::size_t skeleton = 0; skeleton < joints.size(); ++skeleton) {
            out.push_back(bone_order(static_cast<int>(skeleton)));
        }
        return out;
    }

    [[nodiscard]] const Node& node(std::size_t i) const { return nodes_[i]; }

private:
    void sort_by_key(std::vector<std::size_t>& list) const {
        std::ranges::sort(list, {}, [this](std::size_t i) { return nodes_[i].key; });
    }

    // _find_highest_node: the first of the subset nearest the root.
    [[nodiscard]] std::size_t highest(const std::vector<std::size_t>& subset) const {
        std::size_t best = subset.front();
        for (const std::size_t i : subset) {
            if (nodes_[i].height < nodes_[best].height) {
                best = i;
            }
        }
        return best;
    }

    // Adds a node above a skin's joints to it: to its joints if it is another skin's joint.
    void add_parent(Skin& skin, std::size_t parent) const {
        if (nodes_[parent].joint && !has(skin.joints, parent)) {
            skin.joints.push_back(parent);
        } else if (!has(skin.non_joints, parent)) {
            skin.non_joints.push_back(parent);
        }
    }

    // _capture_nodes_for_multirooted_skin: the nodes up to where a skin's separate trees meet.
    void capture_multirooted(Skin& skin) const {
        DisjointSet sets;
        for (const std::size_t joint : skin.joints) {
            sets.insert(joint);
            const int parent = nodes_[joint].parent;
            if (parent >= 0 && has(skin.joints, static_cast<std::size_t>(parent))) {
                sets.create_union(static_cast<std::size_t>(parent), joint);
            }
        }
        std::vector<std::size_t> roots = sets.representatives();
        if (roots.size() <= 1) {
            return;
        }
        int top = kNone;
        for (const std::size_t root : roots) {
            if (top == kNone || nodes_[root].height < top) {
                top = nodes_[root].height;
            }
        }
        for (std::size_t& root : roots) {
            while (nodes_[root].height > top) {
                const auto parent = static_cast<std::size_t>(nodes_[root].parent);
                add_parent(skin, parent);
                root = parent;
            }
        }
        for (bool same = false; !same;) {
            same = std::ranges::all_of(roots, [&](std::size_t root) {
                return nodes_[root].parent == nodes_[roots.front()].parent;
            });
            if (!same) {
                for (std::size_t& root : roots) {
                    if (nodes_[root].parent < 0) {
                        return; // trees under different scene roots never meet
                    }
                    const auto parent = static_cast<std::size_t>(nodes_[root].parent);
                    add_parent(skin, parent);
                    root = parent;
                }
            }
        }
    }

    // _capture_nodes_in_skin, with Godot's test of a joint's place in the list (> 0, so the
    // first joint doesn't count).
    bool capture_in_skin(Skin& skin, std::size_t index) const {
        bool found = false;
        for (const std::size_t child : nodes_[index].children) {
            found = capture_in_skin(skin, child) || found;
        }
        if (found) {
            if (nodes_[index].joint && !has(skin.joints, index)) {
                skin.joints.push_back(index);
            } else if (!has(skin.non_joints, index)) {
                skin.non_joints.push_back(index);
            }
        }
        const auto at = std::ranges::find(skin.joints, index);
        return at != skin.joints.end() && at != skin.joints.begin();
    }

    void expand_skin(Skin& skin) const {
        capture_multirooted(skin);
        std::vector<std::size_t> all = skin.joints;
        all.insert(all.end(), skin.non_joints.begin(), skin.non_joints.end());
        DisjointSet sets;
        for (const std::size_t i : all) {
            sets.insert(i);
            const int parent = nodes_[i].parent;
            if (parent >= 0 && has(all, static_cast<std::size_t>(parent))) {
                sets.create_union(static_cast<std::size_t>(parent), i);
            }
        }
        std::vector<std::size_t> roots;
        for (const std::size_t owner : sets.representatives()) {
            roots.push_back(highest(sets.members(owner)));
        }
        sort_by_key(roots);
        for (const std::size_t root : roots) {
            capture_in_skin(skin, root);
        }
        skin.roots = std::move(roots);
    }

    // _recurse_children: a node and those under it, but skinned meshes with no children.
    void recurse_children(std::size_t index, std::set<std::size_t>& all,
                          std::set<std::size_t>& visited) const {
        if (!visited.insert(index).second) {
            return;
        }
        const Node& node = nodes_[index];
        for (const std::size_t child : node.children) {
            recurse_children(child, all, visited);
        }
        if (!node.skinned || !node.children.empty()) {
            all.insert(index);
        }
    }

    // _check_if_parent_needs_to_become_joint: the nodes of the skeleton between a joint and the
    // joint above it.
    void parents_become_joints(const std::vector<std::size_t>& skeleton_nodes, std::size_t index,
                               std::vector<std::size_t>& non_joints) const {
        const int parent = nodes_[index].parent;
        if (parent < 0) {
            return;
        }
        const auto p = static_cast<std::size_t>(parent);
        if (!nodes_[p].joint && has(skeleton_nodes, p) && !has(non_joints, p)) {
            parents_become_joints(skeleton_nodes, p, non_joints);
            non_joints.push_back(p);
        }
    }

    // _determine_skeletons, turning no non-joint descendants into bones as glTF files and FBX
    // files of naming version 2 do: each skeleton's joints.
    std::vector<std::vector<std::size_t>> determine_skeletons() {
        DisjointSet sets;
        for (const Skin& skin : skins_) {
            std::set<std::size_t> visited;
            std::set<std::size_t> all; // ordered by index, as Godot's RBSet
            for (const std::size_t i : skin.joints) {
                all.insert(i);
                recurse_children(i, all, visited);
            }
            for (const std::size_t i : skin.non_joints) {
                all.insert(i);
                recurse_children(i, all, visited);
            }
            std::vector<std::size_t> ordered(all.begin(), all.end());
            sort_by_key(ordered);
            for (const std::size_t i : ordered) {
                sets.insert(i);
                const int parent = nodes_[i].parent;
                if (parent >= 0 && all.contains(static_cast<std::size_t>(parent))) {
                    sets.create_union(static_cast<std::size_t>(parent), i);
                }
            }
            for (std::size_t i = 1; i < skin.roots.size(); ++i) {
                sets.create_union(skin.roots.front(), skin.roots[i]);
            }
        }

        // Joins sets whose tops are siblings, or one under another, keeping Godot's loop, which
        // stops at the set itself.
        {
            std::vector<std::vector<std::size_t>> groups;
            std::vector<std::size_t> tops;
            for (const std::size_t owner : sets.representatives()) {
                groups.push_back(sets.members(owner));
                tops.push_back(highest(groups.back()));
            }
            for (std::size_t i = 0; i < tops.size(); ++i) {
                for (std::size_t j = i + 1; j < tops.size(); ++j) {
                    if (nodes_[tops[i]].parent == nodes_[tops[j]].parent) {
                        sets.create_union(tops[i], tops[j]);
                    }
                }
                const int parent = nodes_[tops[i]].parent;
                if (parent >= 0) {
                    for (std::size_t j = 0; j < groups.size() && i != j; ++j) {
                        if (has(groups[j], static_cast<std::size_t>(parent))) {
                            sets.create_union(tops[i], tops[j]);
                        }
                    }
                }
            }
        }

        std::vector<std::vector<std::size_t>> out;
        for (const std::size_t owner : sets.representatives()) {
            const std::vector<std::size_t> skeleton_nodes = sets.members(owner);
            std::vector<std::size_t> joints;
            std::vector<std::size_t> non_joints;
            for (const std::size_t i : skeleton_nodes) {
                if (nodes_[i].joint) {
                    parents_become_joints(skeleton_nodes, i, non_joints);
                    joints.push_back(i);
                }
            }
            // _reparent_non_joint_skeleton_subtrees.
            for (const std::size_t i : non_joints) {
                nodes_[i].joint = true;
                joints.push_back(i);
            }
            out.push_back(std::move(joints));
        }
        return out;
    }

    // _determine_skeleton_roots, then _create_skeletons' walk: depth first, roots and children
    // by their index in the file.
    [[nodiscard]] std::vector<std::size_t> bone_order(int skeleton) const {
        DisjointSet sets;
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].skeleton != skeleton) {
                continue;
            }
            sets.insert(i);
            const int parent = nodes_[i].parent;
            if (parent >= 0 && nodes_[static_cast<std::size_t>(parent)].skeleton == skeleton) {
                sets.create_union(static_cast<std::size_t>(parent), i);
            }
        }
        std::vector<std::size_t> roots;
        for (const std::size_t owner : sets.representatives()) {
            roots.push_back(highest(sets.members(owner)));
        }
        sort_by_key(roots);
        std::vector<std::size_t> bones;
        std::vector<std::size_t> stack(roots.rbegin(), roots.rend());
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            bones.push_back(i);
            std::vector<std::size_t> children;
            for (const std::size_t child : nodes_[i].children) {
                if (nodes_[child].skeleton == skeleton) {
                    children.push_back(child);
                }
            }
            sort_by_key(children);
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
        return bones;
    }

    std::vector<Node> nodes_;
    std::vector<Skin> skins_;
};

} // namespace

std::vector<ModelSkeleton> model_skeletons(const Model& model) {
    if (model.skins.empty()) {
        return {};
    }
    Tool tool(model);
    const std::vector<std::vector<std::size_t>> bones = tool.skeletons();
    std::vector<ModelSkeleton> out(bones.size());
    for (std::size_t i = 0; i < bones.size(); ++i) {
        out[i].bones = bones[i];
    }

    // Where GLTFDocument::_generate_scene_node puts each Skeleton3D: under the scene node of
    // the parent of the first of its bones the walk reaches, named "Skeleton3D" as add_child
    // names it beside another of that name.
    std::map<std::string, std::set<std::string>> skeleton_names; // by parent path
    const auto join = [](const std::string& parent, const std::string& name) {
        return parent.empty() ? name : parent + "/" + name;
    };
    const std::function<void(std::size_t, const std::string&, int)> generate =
        [&](std::size_t index, const std::string& parent_path, int parent_skeleton) {
            const Node& node = tool.node(index);
            const std::string& name = model.nodes[index].name;
            std::string path;
            int skeleton = kNone;
            if (node.skeleton >= 0) {
                skeleton = node.skeleton;
                ModelSkeleton& made = out[static_cast<std::size_t>(skeleton)];
                if (parent_skeleton != skeleton && made.path.empty()) {
                    // A skeleton right under another's bone hangs from a BoneAttachment3D.
                    const std::string under =
                        parent_skeleton >= 0 ? join(parent_path, "BoneAttachment3D") : parent_path;
                    std::set<std::string>& taken = skeleton_names[under];
                    std::string child = "Skeleton3D";
                    for (int n = 2; taken.contains(child); ++n) {
                        child = "Skeleton3D" + std::to_string(n);
                    }
                    taken.insert(child);
                    made.path = join(under, child);
                }
                path = made.path;
            } else {
                // A node under a bone hangs from a BoneAttachment3D named after it.
                const bool attached = parent_skeleton >= 0 && !node.skinned;
                path = join(attached ? join(parent_path, name) : parent_path, name);
            }
            for (const std::size_t child : node.children) {
                generate(child, path, skeleton);
            }
        };
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes[i].parent < 0) {
            generate(i, "", kNone);
        }
    }
    return out;
}

} // namespace slash::scene3d
