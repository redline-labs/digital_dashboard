// A `capnp compile` plugin that generates the pub/sub schema registry.
//
//     capnp compile -o/path/to/capnpc_schema_registry:<outdir> foo.capnp bar.capnp
//
// A capnp plugin is just an executable that reads a CodeGeneratorRequest on
// stdin -- the same interface capnpc-c++ uses. `capnp` chdir()s us into the
// directory given after the ':', so everything below writes relative paths.
//
// It exists because the thing it replaces did not work. The registry used to be
// built by a CMake regex over the schema *text*
// (`string(REGEX MATCHALL "struct[ \t]+([a-zA-Z_][a-zA-Z0-9_]*)[ \t]" ...)`),
// which got three things wrong and only announced one of them:
//
//   struct Tight{          silently dropped -- the pattern needs whitespace
//                          after the name, so the schema compiled, generated,
//                          and was simply absent from the registry. Nothing
//                          failed until a subscriber could not decode a topic.
//   # ...struct Ghost...   matched, because comments were never stripped
//     struct Inner {}      matched unqualified, so the generated
//                          capnp::Schema::from<Inner>() did not compile
//
// The last one drove schema design: schemas were written flat because nesting
// broke the build. Here capnp does the parsing, so none of that applies -- what
// we get handed is the parsed schema graph, and a name we cannot derive is a
// hard error rather than a missing entry.
//
// Generates, into the output directory:
//
//   pub_sub/schema_registry.h    enum + traits + declarations
//   schema_registry.cpp          the capnp::Schema::from<>() table
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <capnp/message.h>
#include <capnp/schema.capnp.h>
#include <capnp/serialize.h>

#include <kj/filesystem.h>
#include <kj/main.h>
#include <kj/string.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace
{

// $Cxx.namespace, from capnp's own c++.capnp. No schema in this tree sets it
// (everything lands at global scope), but a file that did would have its C++
// types somewhere other than where we guessed, so it is worth detecting.
constexpr uint64_t kCxxNamespaceAnnotationId = 0xb9c6f99ebf805f2cull;

struct Entry
{
    // The one name for this schema: the enum identifier, the string in a
    // dashboard config, and the schema half of the zenoh encoding are all this.
    // "CanFrame" at file scope, "Outer_Inner" for a nested struct.
    std::string registry_name;

    // Fully qualified, leading "::" -- the generated table says
    // capnp::Schema::from<::CanFrame>() so that a schema sharing a name with
    // something in namespace pub_sub still resolves to the capnp type.
    std::string cxx_name;

    // capnp's own id and display name for this node, e.g.
    // "can_frame.capnp:CanFrame".
    //
    // Needed for the schema DESCRIPTOR -- the serialized CodeGeneratorRequest
    // emitted alongside each entry so that a consumer outside this build (an
    // MCAP reader, Foxglove Studio) can decode a recorded message without our
    // generated C++ headers. The display name is what such a consumer resolves
    // the root node by; the id is where the transitive closure starts.
    uint64_t node_id = 0;
    std::string display_name;
};

struct SourceFile
{
    // As capnp reports it, i.e. relative to --src-prefix: "can_frame.capnp".
    std::string filename;

    // Top-level struct names, which the header can forward declare.
    std::vector<std::string> forward_declarable;

    // Set when the header has to #include this file instead of forward
    // declaring it: a nested struct cannot be named without its enclosing
    // class definition, and a $Cxx.namespace puts the types somewhere a bare
    // global-scope declaration would not match. Neither happens in this tree
    // today, so in practice the header forward declares everything.
    bool needs_include = false;
};

class SchemaRegistryMain
{
public:
    explicit SchemaRegistryMain(kj::ProcessContext& context) : context_(context) {}

    kj::MainFunc getMain()
    {
        return kj::MainBuilder(context_, "capnpc_schema_registry",
                               "Generates pub_sub/schema_registry.h and schema_registry.cpp "
                               "from the schemas passed to `capnp compile`. Not meant to be run "
                               "by hand; see schemas/CMakeLists.txt.")
            .callAfterParsing(KJ_BIND_METHOD(*this, run))
            .build();
    }

private:
    kj::ProcessContext& context_;

    // Every node in the request, by id, so nestedNodes can be followed.
    std::map<uint64_t, capnp::schema::Node::Reader> nodes_;

    // In request order, which is command-line order, which is the sorted glob
    // in schemas/CMakeLists.txt. Generated output is therefore stable across
    // builds and diffs cleanly when a schema is added.
    std::vector<Entry> entries_;
    std::vector<SourceFile> files_;

    // registry_name -> the file that claimed it first.
    std::map<std::string, std::string> claimed_names_;

    kj::MainBuilder::Validity run()
    {
        capnp::ReaderOptions options;
        options.traversalLimitInWords = 1 << 30;
        capnp::StreamFdMessageReader reader(0, options);
        auto request = reader.getRoot<capnp::schema::CodeGeneratorRequest>();

        for (auto node : request.getNodes())
        {
            nodes_.emplace(node.getId(), node);
        }

        for (auto requested : request.getRequestedFiles())
        {
            const auto found = nodes_.find(requested.getId());
            if (found == nodes_.end())
            {
                return kj::str("requested file ", requested.getFilename(),
                               " is missing from the request");
            }
            const capnp::schema::Node::Reader file_node = found->second;

            // capnpc-c++ names its outputs after the file node's displayName,
            // so taking the include name from the same place is what keeps the
            // two agreeing about "can_frame.capnp.h".
            SourceFile file;
            file.filename = toStd(file_node.getDisplayName());

            const std::string cxx_namespace = cxxNamespaceOf(file_node);
            file.needs_include = !cxx_namespace.empty();

            files_.push_back(std::move(file));
            if (const auto error = collect(file_node, "", cxx_namespace, files_.back().filename,
                                           files_.size() - 1, true))
            {
                return kj::str(*error);
            }
        }

        if (entries_.empty())
        {
            return kj::str("no struct types found -- refusing to generate an empty registry");
        }

        writeFile("pub_sub/schema_registry.h", header());
        writeFile("schema_registry.cpp", source());
        return true;
    }

    // Walks a scope, appending an entry for every struct and recursing. Enums,
    // consts, interfaces and annotations are skipped: none of them can be the
    // root of a message, so none of them can be published.
    //
    // Returns an error message, or nullopt on success.
    std::optional<std::string> collect(capnp::schema::Node::Reader scope,
                                       const std::string& registry_prefix,
                                       const std::string& cxx_prefix,
                                       const std::string& filename,
                                       size_t file_index,
                                       bool at_file_scope)
    {
        for (auto nested : scope.getNestedNodes())
        {
            const auto found = nodes_.find(nested.getId());
            if (found == nodes_.end())
            {
                continue;  // Declared in an import we were not asked to generate.
            }
            const capnp::schema::Node::Reader node = found->second;
            const std::string name = toStd(nested.getName());

            // An ANNOTATION is not a type. capnp spells annotations in
            // lowerCamelCase by convention -- `fixedLength` -- and capnpc-c++
            // generates no C++ type for one, so the uppercase rule below does
            // not apply and enforcing it would make it impossible to declare an
            // annotation anywhere in this tree.
            //
            // Skipped entirely rather than merely exempted from the name check:
            // there is no struct or enum here to put in the registry either.
            if (node.isAnnotation())
            {
                continue;
            }

            // capnp requires type names to start with a capital letter and every
            // C++ keyword is lowercase, so the keyword mangling capnpc-c++ does
            // (appending '_') cannot trigger here. Check the invariant rather
            // than reimplement its keyword table -- if capnp ever relaxes this,
            // the build stops instead of emitting a reference to a type spelled
            // differently in the generated header.
            if (name.empty() || name[0] < 'A' || name[0] > 'Z')
            {
                return "in " + filename + ": type name '" + name +
                       "' does not start with an uppercase letter, so the C++ name capnpc-c++ "
                       "generates for it cannot be derived safely";
            }

            const std::string registry_name =
                registry_prefix.empty() ? name : registry_prefix + "_" + name;
            const std::string cxx_name = cxx_prefix + "::" + name;

            // Groups are reached through fields, not nestedNodes, so this is
            // belt and braces -- but a group is not a standalone type and
            // capnp::Schema::from<> would not compile for one.
            if (node.isStruct() && !node.getStruct().getIsGroup())
            {
                const auto claim = claimed_names_.find(registry_name);
                if (claim != claimed_names_.end())
                {
                    return "duplicate schema name '" + registry_name + "': declared in " +
                           claim->second + " and again in " + filename +
                           ". Schema names are the identity of a message on the bus and in "
                           "dashboard configs, so they have to be unique across all of "
                           "schemas/*.capnp -- rename one of them.";
                }
                claimed_names_.emplace(registry_name, filename);
                entries_.push_back(
                    Entry{registry_name, cxx_name, node.getId(),
                          toStd(node.getDisplayName())});

                if (at_file_scope)
                {
                    files_[file_index].forward_declarable.push_back(name);
                }
                else
                {
                    files_[file_index].needs_include = true;
                }
            }

            if (const auto error =
                    collect(node, registry_name, cxx_name, filename, file_index, false))
            {
                return error;
            }
        }
        return std::nullopt;
    }


    // ------------------------------------------------------ schema descriptors
    //
    // A serialized capnp CodeGeneratorRequest per schema, so that a consumer
    // OUTSIDE this build can decode one of our messages.
    //
    // Why this belongs here and nowhere else: a CodeGeneratorRequest is exactly
    // what a capnp plugin is handed, and this is the only place in the tree that
    // has one. Reconstructing an equivalent at runtime from capnp::Schema would
    // mean rebuilding the node graph field by field -- a second implementation of
    // something capnp already gave us, with its own bugs.
    //
    // It is PRUNED to the transitive closure of each schema rather than emitted
    // whole. The full request for schemas/*.capnp is on the order of a hundred
    // kilobytes; a bag file writes one descriptor per schema it records, so
    // emitting the whole thing each time would put tens of megabytes of
    // duplicated node graph into every recording.

    // Every node id reachable from `root`, plus the scope chain up to the file
    // node.
    //
    // A consumer loads these into a capnp::SchemaLoader, which resolves a field's
    // type by id -- so a type reachable from a field and NOT in this set makes
    // that field undecodable. The failure is not an error at load time; it
    // surfaces later as a missing type, which is why the closure is computed
    // rather than approximated.
    void addClosure(uint64_t id, std::set<uint64_t>& out) const
    {
        if (id == 0 || out.count(id) != 0)
        {
            return;
        }
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
        {
            // A node from an import we were not asked to generate. Nothing to
            // add, and nothing we could add.
            return;
        }
        out.insert(id);

        const capnp::schema::Node::Reader node = found->second;

        // Upwards: the enclosing scope, ending at the file node. A node whose
        // scope is absent still loads, but its displayNamePrefixLength refers to
        // a scope nothing can look up, so a consumer printing qualified names
        // gets them wrong.
        addClosure(node.getScopeId(), out);

        if (node.isStruct())
        {
            for (auto field : node.getStruct().getFields())
            {
                if (field.isGroup())
                {
                    // A group is a node in its own right, reached only through
                    // the field -- never through nestedNodes.
                    addClosure(field.getGroup().getTypeId(), out);
                }
                else if (field.isSlot())
                {
                    addTypeClosure(field.getSlot().getType(), out);
                }
            }
        }
    }

    void addTypeClosure(capnp::schema::Type::Reader type, std::set<uint64_t>& out) const
    {
        switch (type.which())
        {
            case capnp::schema::Type::STRUCT:
                addClosure(type.getStruct().getTypeId(), out);
                break;
            case capnp::schema::Type::ENUM:
                addClosure(type.getEnum().getTypeId(), out);
                break;
            case capnp::schema::Type::INTERFACE:
                addClosure(type.getInterface().getTypeId(), out);
                break;
            case capnp::schema::Type::LIST:
                // List<List<Foo>> is legal, so this recurses rather than
                // inspecting one level.
                addTypeClosure(type.getList().getElementType(), out);
                break;

            // Everything else is a primitive with no node behind it. Spelled out
            // rather than defaulted so that a capnp release adding a type kind
            // fails this build instead of silently omitting it from the closure
            // -- which would produce a descriptor that loads and then cannot
            // decode one field.
            case capnp::schema::Type::VOID:
            case capnp::schema::Type::BOOL:
            case capnp::schema::Type::INT8:
            case capnp::schema::Type::INT16:
            case capnp::schema::Type::INT32:
            case capnp::schema::Type::INT64:
            case capnp::schema::Type::UINT8:
            case capnp::schema::Type::UINT16:
            case capnp::schema::Type::UINT32:
            case capnp::schema::Type::UINT64:
            case capnp::schema::Type::FLOAT32:
            case capnp::schema::Type::FLOAT64:
            case capnp::schema::Type::TEXT:
            case capnp::schema::Type::DATA:
            case capnp::schema::Type::ANY_POINTER:
                break;
        }
    }

    // The pruned request for one entry, serialized.
    kj::Array<capnp::word> descriptorFor(const Entry& entry) const
    {
        std::set<uint64_t> closure;
        addClosure(entry.node_id, closure);

        capnp::MallocMessageBuilder message;
        auto request = message.initRoot<capnp::schema::CodeGeneratorRequest>();

        auto nodes = request.initNodes(static_cast<unsigned>(closure.size()));
        unsigned index = 0;
        uint64_t file_id = 0;
        for (const uint64_t id : closure)
        {
            const auto found = nodes_.find(id);
            nodes.setWithCaveats(index++, found->second);
            if (found->second.isFile())
            {
                file_id = id;
            }
        }

        // requestedFiles is what tells a consumer which file this descriptor is
        // "about". Foxglove and capnp's own tooling both read it.
        if (file_id != 0)
        {
            auto files = request.initRequestedFiles(1);
            files[0].setId(file_id);
            files[0].setFilename(nodes_.at(file_id).getDisplayName());
        }

        return capnp::messageToFlatArray(message);
    }

    // The descriptors as C++ byte-array literals.
    std::string descriptorTable() const
    {
        std::string out =
            "// Serialized capnp CodeGeneratorRequests, pruned to each schema's transitive\n"
            "// closure. See the generator for why these exist and why they are pruned.\n"
            "namespace\n"
            "{\n";

        for (const auto& entry : entries_)
        {
            const kj::Array<capnp::word> words = descriptorFor(entry);
            const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();

            out += "\nconstexpr std::uint8_t kDescriptor_" + entry.registry_name + "[] = {";
            for (size_t i = 0; i < bytes.size(); ++i)
            {
                if (i % 16 == 0)
                {
                    out += "\n    ";
                }
                out += std::to_string(static_cast<unsigned>(bytes[i])) + ",";
            }
            out += "\n};\n";
        }

        out += "\n}  // namespace\n\n";
        return out;
    }

    static std::string cxxNamespaceOf(capnp::schema::Node::Reader file)
    {
        for (auto annotation : file.getAnnotations())
        {
            if (annotation.getId() == kCxxNamespaceAnnotationId)
            {
                return "::" + toStd(annotation.getValue().getText());
            }
        }
        return "";
    }

    static std::string toStd(kj::StringPtr text) { return std::string(text.cStr(), text.size()); }

    static constexpr const char* kBanner =
        "// GENERATED by schemas/tools/schema_registry_plugin.cpp -- DO NOT EDIT.\n"
        "//\n"
        "// Regenerated on every build from schemas/*.capnp. To add a schema, drop the\n"
        "// .capnp file into schemas/ and build; nothing here or in CMake needs editing.\n"
        "//\n"
        "// SPDX-License-Identifier: GPL-3.0-or-later\n";

    std::string header() const
    {
        std::string out = kBanner;
        out +=
            "\n"
            "#ifndef PUB_SUB_SCHEMA_REGISTRY_H_\n"
            "#define PUB_SUB_SCHEMA_REGISTRY_H_\n"
            "\n"
            "#include <array>\n"
            "#include <cstdint>\n"
            "#include <span>\n"
            "#include <optional>\n"
            "#include <string_view>\n"
            "\n"
            "#include <capnp/schema.h>\n"
            "\n"
            "#include \"reflection/reflection.h\"\n"
            "\n"
            "// Forward declarations, not #include \"<name>.capnp.h\".\n"
            "//\n"
            "// Every widget's config.h includes this header, so what it drags in is paid\n"
            "// for across the whole dashboard. #including the generated capnp headers here\n"
            "// cost 16,190 preprocessed lines on top of what this header actually needs;\n"
            "// forward declaring costs 415.\n"
            "//\n"
            "// An explicit specialization of schema_traits below only needs the type\n"
            "// *declared*, and every translation unit that actually builds or reads a\n"
            "// message already includes its own .capnp.h -- so the definitions are only\n"
            "// needed in schema_registry.cpp, which is the one place that has them.\n"
            "//\n"
            "// A file with a nested struct or a $Cxx.namespace gets included instead: a\n"
            "// nested type cannot be named without its enclosing class definition.\n";

        for (const auto& file : files_)
        {
            if (file.needs_include)
            {
                out += "#include \"" + file.filename + ".h\"\n";
            }
        }
        for (const auto& file : files_)
        {
            if (file.needs_include)
            {
                continue;
            }
            for (const auto& name : file.forward_declarable)
            {
                out += "struct " + name + ";\n";
            }
        }

        out +=
            "\n"
            "namespace pub_sub\n"
            "{\n"
            "\n"
            "// Written out rather than declared with REFLECT_ENUM: that macro tops out at\n"
            "// 96 entries and fails as a wall of macro errors on the 97th. This is what it\n"
            "// expands to anyway -- the enum plus the two ADL hooks reflection::enum_traits\n"
            "// looks for -- so spelling it directly removes the ceiling and a layer of\n"
            "// indirection at the same time.\n"
            "enum class schema_type_t\n"
            "{\n";
        for (const auto& entry : entries_)
        {
            out += "    " + entry.registry_name + ",\n";
        }
        out +=
            "};\n"
            "\n"
            "constexpr auto enum_names(schema_type_t)\n"
            "{\n"
            "    return std::array{\n";
        for (const auto& entry : entries_)
        {
            out += "        std::string_view{\"" + entry.registry_name + "\"},\n";
        }
        out +=
            "    };\n"
            "}\n"
            "\n"
            "constexpr auto enum_values(schema_type_t)\n"
            "{\n"
            "    return std::array{\n";
        for (const auto& entry : entries_)
        {
            out += "        schema_type_t::" + entry.registry_name + ",\n";
        }
        out +=
            "    };\n"
            "}\n"
            "\n"
            "// Every schema name known to this build, in declaration order.\n"
            "constexpr auto get_available_schemas()\n"
            "{\n"
            "    return reflection::enum_traits<schema_type_t>::names();\n"
            "}\n"
            "\n"
            "// nullopt when nothing is registered under that name or type -- which is the\n"
            "// normal answer for a payload published by something outside this build, not\n"
            "// an error. Callers decide whether to fall back or complain.\n"
            "std::optional<capnp::Schema> get_schema(schema_type_t schema_type);\n"
            "std::optional<capnp::Schema> get_schema(std::string_view schema_name);\n"
            "\n"
            "// The schema as a serialized capnp CodeGeneratorRequest, pruned to its\n"
            "// transitive closure.\n"
            "//\n"
            "// This is what lets something OUTSIDE this build decode one of our messages:\n"
            "// capnp::Schema above is a handle into code the compiler generated for us, and\n"
            "// is meaningless to anyone who does not link it. A descriptor is the schema as\n"
            "// DATA -- loadable by capnp::SchemaLoader, embeddable in a recording, and the\n"
            "// exact form MCAP registers for schema_encoding \"capnproto\".\n"
            "//\n"
            "// Empty when the name is unknown. The bytes have static storage duration and\n"
            "// live for the life of the process.\n"
            "std::span<const std::uint8_t> schema_descriptor(schema_type_t schema_type);\n"
            "std::span<const std::uint8_t> schema_descriptor(std::string_view schema_name);\n"
            "\n"
            "// capnp's own qualified name, e.g. \"can_frame.capnp:CanFrame\".\n"
            "//\n"
            "// NOT the registry name. A consumer reading a descriptor resolves the root node\n"
            "// by this, because it is what capnp records in the node graph -- the registry\n"
            "// name is ours alone and appears nowhere in the descriptor.\n"
            "std::string_view schema_display_name(schema_type_t schema_type);\n"
            "\n"
            "// Specialized only for registered schemas, so publishing an unregistered type\n"
            "// is a compile error rather than a message nothing can decode.\n"
            "template <typename Schema>\n"
            "struct schema_traits;\n"
            "\n";
        for (const auto& entry : entries_)
        {
            out += "template <> struct schema_traits<" + entry.cxx_name +
                   "> { static constexpr std::string_view name = \"" + entry.registry_name +
                   "\"; };\n";
        }
        out +=
            "\n"
            "}  // namespace pub_sub\n"
            "\n"
            "#endif  // PUB_SUB_SCHEMA_REGISTRY_H_\n";
        return out;
    }

    std::string source() const
    {
        std::string out = kBanner;
        out += "\n#include \"pub_sub/schema_registry.h\"\n\n";
        for (const auto& file : files_)
        {
            out += "#include \"" + file.filename + ".h\"\n";
        }
        out += "\n";
        out += descriptorTable();
        out +=
            "namespace pub_sub\n"
            "{\n"
            "\n"
            "// A switch rather than a table so that -Wswitch-enum -Werror, which this tree\n"
            "// builds with, fails the build if the enum above and the cases below ever\n"
            "// disagree. They are generated together, so that is a guard against this\n"
            "// generator, not against a human.\n"
            "std::optional<capnp::Schema> get_schema(schema_type_t schema_type)\n"
            "{\n"
            "    switch (schema_type)\n"
            "    {\n";
        for (const auto& entry : entries_)
        {
            out += "        case schema_type_t::" + entry.registry_name +
                   ": return capnp::Schema::from<" + entry.cxx_name + ">();\n";
        }
        out +=
            "    }\n"
            "\n"
            "    return std::nullopt;\n"
            "}\n"
            "\n"
            "// Delegates rather than repeating the table, so the name a config uses and the\n"
            "// name on the wire cannot resolve to a different schema than the enum does.\n"
            "std::optional<capnp::Schema> get_schema(std::string_view schema_name)\n"
            "{\n"
            "    if (const auto schema_type = "
            "reflection::enum_traits<schema_type_t>::try_from_string(schema_name))\n"
            "    {\n"
            "        return get_schema(*schema_type);\n"
            "    }\n"
            "\n"
            "    return std::nullopt;\n"
            "}\n"
            "\n"
            "// Same switch shape as get_schema, and for the same reason: -Wswitch-enum\n"
            "// fails the build if the enum and the cases drift apart.\n"
            "std::span<const std::uint8_t> schema_descriptor(schema_type_t schema_type)\n"
            "{\n"
            "    switch (schema_type)\n"
            "    {\n";
        for (const auto& entry : entries_)
        {
            out += "        case schema_type_t::" + entry.registry_name +
                   ": return kDescriptor_" + entry.registry_name + ";\n";
        }
        out +=
            "    }\n"
            "\n"
            "    return {};\n"
            "}\n"
            "\n"
            "std::span<const std::uint8_t> schema_descriptor(std::string_view schema_name)\n"
            "{\n"
            "    if (const auto schema_type = "
            "reflection::enum_traits<schema_type_t>::try_from_string(schema_name))\n"
            "    {\n"
            "        return schema_descriptor(*schema_type);\n"
            "    }\n"
            "\n"
            "    return {};\n"
            "}\n"
            "\n"
            "std::string_view schema_display_name(schema_type_t schema_type)\n"
            "{\n"
            "    switch (schema_type)\n"
            "    {\n";
        for (const auto& entry : entries_)
        {
            out += "        case schema_type_t::" + entry.registry_name + ": return \"" +
                   entry.display_name + "\";\n";
        }
        out +=
            "    }\n"
            "\n"
            "    return {};\n"
            "}\n"
            "\n"
            "}  // namespace pub_sub\n";
        return out;
    }

    static void writeFile(kj::StringPtr filename, const std::string& text)
    {
        auto fs = kj::newDiskFilesystem();
        auto file = fs->getCurrent().openFile(kj::Path::parse(filename),
                                              kj::WriteMode::CREATE | kj::WriteMode::MODIFY |
                                                  kj::WriteMode::CREATE_PARENT);
        file->writeAll(kj::ArrayPtr<const kj::byte>(
            reinterpret_cast<const kj::byte*>(text.data()), text.size()));
    }
};

}  // namespace

KJ_MAIN(SchemaRegistryMain);
