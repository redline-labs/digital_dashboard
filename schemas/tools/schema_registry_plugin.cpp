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

#include <capnp/schema.capnp.h>
#include <capnp/serialize.h>

#include <kj/filesystem.h>
#include <kj/main.h>
#include <kj/string.h>

#include <map>
#include <optional>
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
                entries_.push_back(Entry{registry_name, cxx_name});

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
        out +=
            "\n"
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
