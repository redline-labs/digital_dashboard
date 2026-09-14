#ifndef SWITCHBOARD_SCHEMA_FORM_H_
#define SWITCHBOARD_SCHEMA_FORM_H_

#include <nlohmann/json.hpp>

#include <capnp/schema.h>

#include <QWidget>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace switchboard
{

using json = nlohmann::json;

class FieldEditor;

// An editable form for one capnp struct, built at RUNTIME from its schema.
//
// The editor's property panel does the same job at compile time over
// REFLECT_STRUCT types (dashboard/editor/properties_panel.cpp). That cannot work
// here: a service's request type is known only by the name its advertisement
// carries, so the form walks capnp::StructSchema instead. It copies the editor's
// choices -- one widget per scalar, nested structs inset, lists with add and
// remove, `field:<path>` object names, the same red invalid border -- and none
// of its code.
//
// THE FORM IS JSON-SHAPED ON PURPOSE. value() is exactly what
// pub_sub::jsonToCapnp takes, and validation IS jsonToCapnp: the form builds a
// throwaway message from its own value on every edit and shows whatever that
// rejects, next to the field it names. So what the form calls valid and what
// the send path accepts cannot drift apart -- there is one set of rules, and
// the form does not have a copy of it.
//
// Field paths are jsonToCapnp's: `nominalBps`, `inner.count`, `via[2].lat`,
// `choice.speed`. Every input widget's objectName is `field:<path>`.
class SchemaForm : public QWidget
{
    Q_OBJECT

  public:
    explicit SchemaForm(QWidget* parent = nullptr);
    ~SchemaForm() override;

    // Rebuilds the form for `schema`, populated with its declared defaults.
    // nullopt empties it.
    void setSchema(std::optional<capnp::StructSchema> schema);
    bool hasSchema() const;

    // Every field, as jsonToCapnp takes it. An empty object with no schema.
    json value() const;

    // Sets the fields `fields` names and leaves the rest as they are, at every
    // level of nesting; a list or a union is replaced whole. ALL OR NOTHING: if
    // the result would not be a valid request, nothing changes and `errors`
    // says why -- including a field name the schema does not have, which is an
    // error rather than ignored.
    bool setValue(const json& fields, std::vector<std::string>& errors);

    // Back to the schema's declared defaults.
    void resetToDefaults();

    bool isValid() const;

    // Why not, one line per problem, each starting with the field path. Empty
    // when valid.
    const std::vector<std::string>& problems() const;

  signals:
    void validityChanged(bool valid);

    // Any change a user or setValue() made.
    void edited();

  private:
    void revalidate();

    // The schema's declared defaults, read back from an untouched message.
    json defaults() const;

    std::optional<capnp::StructSchema> schema_;
    std::unique_ptr<FieldEditor> root_;
    QWidget* container_ = nullptr;
    std::vector<std::string> problems_;
    bool valid_ = true;

    // Set while setValue() applies fields, so the per-field change callbacks do
    // not each revalidate a half-applied form.
    bool applying_ = false;
};

// "UInt16", "List(Text)", "MapLeg", "Colour" -- how a type is written in the
// schema, for the hint next to each field.
std::string capnpTypeName(const capnp::Type& type);

}  // namespace switchboard

#endif  // SWITCHBOARD_SCHEMA_FORM_H_
