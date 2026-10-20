#pragma once

#include "slash/core/result.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small XML reader and writer for the files Android libraries carry: AndroidManifest.xml,
// Maven POMs and maven-metadata.xml. Enough of XML 1.0 for those: elements, attributes, text,
// CDATA, comments, processing instructions and a DOCTYPE without an internal subset; the five
// predefined entities and character references. Names keep their prefixes ("android:name").
namespace slash::exporter {

struct XmlElement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes; // in order, values unescaped
    std::vector<XmlElement> children;
    std::string text; // the character data directly inside, without leading and trailing space

    [[nodiscard]] const std::string* attribute(std::string_view attribute_name) const;
    void set_attribute(std::string_view attribute_name, std::string value);
    void remove_attribute(std::string_view attribute_name);
    // The first child with this name, or nullptr.
    [[nodiscard]] const XmlElement* child(std::string_view child_name) const;
    // The text of the first child with this name; "" when there is none.
    [[nodiscard]] std::string child_text(std::string_view child_name) const;
};

// The document's root element. The error names the line of the first problem.
[[nodiscard]] Result<XmlElement> parse_xml(std::string_view text);

// The document with an XML declaration, one element per line, indented by four spaces.
[[nodiscard]] std::string write_xml(const XmlElement& root);

// text with &, <, >, " and ' escaped, for attribute values and character data.
[[nodiscard]] std::string xml_escaped(std::string_view text);

} // namespace slash::exporter
