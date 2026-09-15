#include "switchboard/schema_form.h"

#include "helpers/hex.h"
#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_registry.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>

namespace switchboard
{

namespace
{

// The editor's invalid-field look (dashboard/editor/properties_panel.cpp), so a
// bad value reads the same in every app in the tree.
constexpr const char* kInvalidStyle = "border: 1px solid #C0392B; background: #2B1A18;";

using Changed = std::function<void()>;

// Data defaults and Data inside a list element's defaults: all of it, as hex.
// A form is only ever built for a request, which is small by nature.
const pub_sub::CapnpJsonOptions kAllDataAsHex{.data_hex_limit = std::numeric_limits<std::size_t>::max()};

std::string childPath(const std::string& parent, const std::string& name)
{
    return parent.empty() ? name : parent + "." + name;
}

bool isUnionGroup(capnp::StructSchema schema)
{
    return schema.getProto().getStruct().getDiscriminantCount() > 0;
}

// Fields in the order the schema text declares them, which is how a person
// reading the .capnp file expects to see them -- not ordinal order, which
// interleaves union arms and later additions.
template <typename List>
std::vector<capnp::StructSchema::Field> inCodeOrder(List fields)
{
    // A loop, not the iterator-pair constructor: capnp's list iterators do not
    // model std::input_iterator.
    std::vector<capnp::StructSchema::Field> out;
    out.reserve(fields.size());
    for (auto field : fields)
    {
        out.push_back(field);
    }
    std::sort(out.begin(), out.end(),
              [](const capnp::StructSchema::Field& a, const capnp::StructSchema::Field& b)
              { return a.getProto().getCodeOrder() < b.getProto().getCodeOrder(); });
    return out;
}

// What a freshly added element of `type` starts as: the schema's own defaults
// for a struct, zero or empty otherwise.
json defaultFor(const capnp::Type& type)
{
    using W = capnp::schema::Type::Which;
    switch (type.which())
    {
        case W::STRUCT:
        {
            capnp::MallocMessageBuilder message;
            auto root = message.initRoot<capnp::DynamicStruct>(type.asStruct());
            return pub_sub::capnpToJson(root.asReader(), kAllDataAsHex);
        }
        case W::LIST:
            return json::array();
        case W::ENUM:
        {
            const auto enumerants = type.asEnum().getEnumerants();
            return enumerants.size() > 0 ? json(enumerants[0].getProto().getName().cStr())
                                         : json("");
        }
        case W::TEXT:
        case W::DATA:
            return "";
        case W::BOOL:
            return false;
        case W::INT8:
        case W::INT16:
        case W::INT32:
        case W::INT64:
        case W::UINT8:
        case W::UINT16:
        case W::UINT32:
        case W::UINT64:
            return 0;
        case W::FLOAT32:
        case W::FLOAT64:
            return 0.0;
        case W::VOID:
        case W::INTERFACE:
        case W::ANY_POINTER:
            return nullptr;
    }
    return nullptr;
}

std::string shortestDouble(double value)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

}  // namespace

// ------------------------------------------------------------------ editors

// One node of the form: a scalar, a struct, a list or a union. The tree of
// these mirrors the JSON value, and each knows its jsonToCapnp path.
class FieldEditor
{
  public:
    FieldEditor(std::string path, Changed changed) :
        path_(std::move(path)),
        changed_(std::move(changed))
    {
    }

    virtual ~FieldEditor()
    {
        // A widget can outlive its editor by an event-loop turn (rows are
        // removed with deleteLater, since the button that removed one is
        // inside it). Nothing that widget emits in that turn may reach us.
        for (const auto& connection : connections_)
        {
            QObject::disconnect(connection);
        }
    }

    FieldEditor(const FieldEditor&) = delete;
    FieldEditor& operator=(const FieldEditor&) = delete;

    const std::string& path() const { return path_; }

    virtual QWidget* widget() = 0;

    // As jsonToCapnp takes it.
    virtual json value() const = 0;

    // Interface and AnyPointer fields have no JSON spelling; a struct leaves
    // them out of its value rather than sending something jsonToCapnp refuses.
    virtual bool settable() const { return true; }

    // Applies an already-validated value. Structs merge; everything else is
    // replaced. Signals from the widgets are blocked while it runs.
    virtual void apply(const json& value) = 0;

    // A problem this editor can see without capnp: text that is not a number,
    // hex that is not hex. Takes priority over whatever jsonToCapnp says about
    // the same path, since "expected an integer" is less useful than "'1O' is
    // not an integer".
    virtual std::optional<std::string> parseProblem() const { return std::nullopt; }

    // Shows (or clears) a problem on this editor's own input.
    virtual void showProblem(const std::optional<std::string>& /*problem*/) {}

    // This editor and every editor below it.
    virtual void visit(const std::function<void(FieldEditor&)>& visitor) { visitor(*this); }

  protected:
    void notifyChanged() const
    {
        if (changed_)
        {
            changed_();
        }
    }

    void track(QMetaObject::Connection connection) { connections_.push_back(std::move(connection)); }

    std::string objectName() const { return "field:" + path_; }

    std::string path_;
    Changed changed_;

  private:
    std::vector<QMetaObject::Connection> connections_;
};

std::unique_ptr<FieldEditor> makeEditor(const capnp::Type& type,
                                        const capnp::StructSchema::Field* field,
                                        const std::string& path, const Changed& changed);

namespace
{

// Shared by every editor that is one QLineEdit.
class LineEditor : public FieldEditor
{
  public:
    LineEditor(std::string path, Changed changed, const QString& placeholder) :
        FieldEditor(std::move(path), std::move(changed))
    {
        edit_ = new QLineEdit();
        edit_->setObjectName(QString::fromStdString(objectName()));
        edit_->setPlaceholderText(placeholder);
        track(QObject::connect(edit_, &QLineEdit::textChanged, [this]() { onText(); }));
    }

    QWidget* widget() override { return edit_; }

    void showProblem(const std::optional<std::string>& problem) override
    {
        edit_->setStyleSheet(problem ? kInvalidStyle : "");
        edit_->setToolTip(problem ? QString::fromStdString(*problem) : QString());
    }

  protected:
    virtual void onText() { notifyChanged(); }

    std::string text() const { return edit_->text().trimmed().toStdString(); }

    void setText(const std::string& text)
    {
        const QSignalBlocker blocker(edit_);
        edit_->setText(QString::fromStdString(text));
    }

    QLineEdit* edit_ = nullptr;
};

class IntegerEditor : public LineEditor
{
  public:
    IntegerEditor(std::string path, Changed changed, const QString& type_name) :
        LineEditor(std::move(path), std::move(changed), type_name)
    {
    }

    json value() const override
    {
        if (const auto parsed = parse())
        {
            return *parsed;
        }
        // Unparseable text goes through as a string: jsonToCapnp rejects it,
        // and parseProblem() has the better message for this path anyway.
        return text();
    }

    void apply(const json& value) override
    {
        if (value.is_number_unsigned())
        {
            setText(std::to_string(value.get<std::uint64_t>()));
        }
        else if (value.is_number_integer())
        {
            setText(std::to_string(value.get<std::int64_t>()));
        }
        else
        {
            setText(value.is_string() ? value.get<std::string>() : value.dump());
        }
    }

    std::optional<std::string> parseProblem() const override
    {
        if (parse())
        {
            return std::nullopt;
        }
        const std::string t = text();
        return t.empty() ? std::string("enter an integer")
                         : "'" + t + "' is not an integer (decimal, or hex with 0x).";
    }

  private:
    // Decimal, or hex with 0x -- command bytes are usually written in hex.
    // Negative values parse signed and the rest unsigned, so both ends of a
    // 64-bit range survive; jsonToCapnp does the range check for the width.
    std::optional<json> parse() const
    {
        std::string t = text();
        if (t.empty())
        {
            return std::nullopt;
        }

        const bool negative = t.front() == '-';
        std::string_view digits(t);
        if (negative)
        {
            digits.remove_prefix(1);
        }
        int base = 10;
        if (digits.starts_with("0x") || digits.starts_with("0X"))
        {
            digits.remove_prefix(2);
            base = 16;
        }
        if (digits.empty())
        {
            return std::nullopt;
        }

        std::uint64_t magnitude = 0;
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), magnitude, base);
        if (error != std::errc() || end != digits.data() + digits.size())
        {
            return std::nullopt;
        }

        if (!negative)
        {
            return json(magnitude);
        }
        const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
        if (magnitude > limit)
        {
            return std::nullopt;
        }
        return json(magnitude == limit ? std::numeric_limits<std::int64_t>::min()
                                       : -static_cast<std::int64_t>(magnitude));
    }
};

class FloatEditor : public LineEditor
{
  public:
    FloatEditor(std::string path, Changed changed, const QString& type_name) :
        LineEditor(std::move(path), std::move(changed), type_name)
    {
    }

    json value() const override
    {
        bool ok = false;
        const double v = edit_->text().trimmed().toDouble(&ok);
        return ok ? json(v) : json(text());
    }

    void apply(const json& value) override
    {
        setText(value.is_number() ? shortestDouble(value.get<double>()) : value.dump());
    }

    std::optional<std::string> parseProblem() const override
    {
        // QString::toDouble is locale-independent, which matters: a user whose
        // locale writes 1,5 should not have 1.5 refused, or 1,5 read as 15.
        bool ok = false;
        edit_->text().trimmed().toDouble(&ok);
        if (ok)
        {
            return std::nullopt;
        }
        const std::string t = text();
        return t.empty() ? std::string("enter a number") : "'" + t + "' is not a number.";
    }
};

class TextEditor : public LineEditor
{
  public:
    TextEditor(std::string path, Changed changed) :
        LineEditor(std::move(path), std::move(changed), "Text")
    {
    }

    json value() const override { return edit_->text().toStdString(); }

    void apply(const json& value) override
    {
        const QSignalBlocker blocker(edit_);
        edit_->setText(QString::fromStdString(value.is_string() ? value.get<std::string>() : ""));
    }
};

// Hex, with the byte count beside it -- the Grayhill request is "up to eight
// bytes", and counting pairs of hex digits by eye is how that goes wrong.
class DataEditor : public LineEditor
{
  public:
    DataEditor(std::string path, Changed changed) :
        LineEditor(std::move(path), std::move(changed), "hex bytes, e.g. 01 ff 7a")
    {
        container_ = new QWidget();
        auto* layout = new QHBoxLayout(container_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(edit_, 1);
        count_ = new QLabel();
        count_->setObjectName(QString::fromStdString(objectName() + ":bytes"));
        count_->setStyleSheet("color: palette(mid); font-size: 11px;");
        layout->addWidget(count_);
        updateCount();
    }

    QWidget* widget() override { return container_; }

    json value() const override { return edit_->text().toStdString(); }

    void apply(const json& value) override
    {
        setText(value.is_string() ? value.get<std::string>() : "");
        updateCount();
    }

    std::optional<std::string> parseProblem() const override
    {
        std::string error;
        if (helpers::fromHex(edit_->text().toStdString(), &error))
        {
            return std::nullopt;
        }
        return error;
    }

  protected:
    void onText() override
    {
        updateCount();
        notifyChanged();
    }

  private:
    void updateCount()
    {
        std::string error;
        const auto bytes = helpers::fromHex(edit_->text().toStdString(), &error);
        count_->setText(bytes ? QString("%1 byte%2").arg(bytes->size()).arg(bytes->size() == 1 ? "" : "s")
                              : QString("—"));
    }

    QWidget* container_ = nullptr;
    QLabel* count_ = nullptr;
};

class BoolEditor : public FieldEditor
{
  public:
    BoolEditor(std::string path, Changed changed) : FieldEditor(std::move(path), std::move(changed))
    {
        check_ = new QCheckBox();
        check_->setObjectName(QString::fromStdString(objectName()));
        track(QObject::connect(check_, &QCheckBox::toggled, [this]() { notifyChanged(); }));
    }

    QWidget* widget() override { return check_; }
    json value() const override { return check_->isChecked(); }

    void apply(const json& value) override
    {
        const QSignalBlocker blocker(check_);
        check_->setChecked(value.is_boolean() && value.get<bool>());
    }

  private:
    QCheckBox* check_ = nullptr;
};

class EnumEditor : public FieldEditor
{
  public:
    EnumEditor(std::string path, Changed changed, capnp::EnumSchema schema) :
        FieldEditor(std::move(path), std::move(changed))
    {
        combo_ = new QComboBox();
        combo_->setObjectName(QString::fromStdString(objectName()));
        for (auto enumerant : schema.getEnumerants())
        {
            combo_->addItem(enumerant.getProto().getName().cStr());
            const std::string_view doc =
                pub_sub::member_doc(schema.getProto().getId(), enumerant.getIndex());
            if (!doc.empty())
            {
                combo_->setItemData(combo_->count() - 1,
                                    QString::fromUtf8(doc.data(), static_cast<qsizetype>(doc.size())),
                                    Qt::ToolTipRole);
            }
        }
        track(QObject::connect(combo_, &QComboBox::currentIndexChanged,
                               [this]() { notifyChanged(); }));
    }

    QWidget* widget() override { return combo_; }
    json value() const override { return combo_->currentText().toStdString(); }

    void apply(const json& value) override
    {
        const QSignalBlocker blocker(combo_);
        if (value.is_string())
        {
            combo_->setCurrentText(QString::fromStdString(value.get<std::string>()));
        }
    }

    void showProblem(const std::optional<std::string>& problem) override
    {
        combo_->setStyleSheet(problem ? kInvalidStyle : "");
    }

  private:
    QComboBox* combo_ = nullptr;
};

// A payload-less union arm: choosing it is the whole value.
class VoidEditor : public FieldEditor
{
  public:
    VoidEditor(std::string path, Changed changed) : FieldEditor(std::move(path), std::move(changed))
    {
        label_ = new QLabel("(no value)");
        label_->setObjectName(QString::fromStdString(objectName()));
        label_->setStyleSheet("color: palette(mid);");
    }

    QWidget* widget() override { return label_; }
    json value() const override { return nullptr; }
    void apply(const json&) override {}

  private:
    QLabel* label_ = nullptr;
};

class UnsupportedEditor : public FieldEditor
{
  public:
    UnsupportedEditor(std::string path, Changed changed) :
        FieldEditor(std::move(path), std::move(changed))
    {
        label_ = new QLabel("not settable from a form");
        label_->setObjectName(QString::fromStdString(objectName()));
        label_->setEnabled(false);
    }

    QWidget* widget() override { return label_; }
    json value() const override { return nullptr; }
    bool settable() const override { return false; }
    void apply(const json&) override {}

  private:
    QLabel* label_ = nullptr;
};

// A field's label: the name, the type as the schema spells it, and an ⓘ that
// carries the schema's doc comment when there is one.
QWidget* makeLabel(const std::string& name, const std::string& type_name, std::string_view doc)
{
    auto* row = new QWidget();
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* title = new QLabel(QString::fromStdString(name));
    title->setStyleSheet("font-weight: 600;");
    layout->addWidget(title);

    auto* type = new QLabel(QString::fromStdString(type_name));
    type->setStyleSheet("color: palette(mid); font-size: 11px;");
    layout->addWidget(type);

    if (!doc.empty())
    {
        const QString text = QString::fromUtf8(doc.data(), static_cast<qsizetype>(doc.size()));
        auto* info = new QLabel("ⓘ");
        info->setToolTip(text);
        info->setStyleSheet("color: palette(mid);");
        layout->addWidget(info);
        title->setToolTip(text);
    }

    layout->addStretch(1);
    return row;
}

class StructEditor : public FieldEditor
{
  public:
    StructEditor(std::string path, Changed changed, capnp::StructSchema schema, bool inset) :
        FieldEditor(std::move(path), std::move(changed))
    {
        auto* frame = new QFrame();
        frame_ = frame;
        auto* form = new QFormLayout(frame);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        if (inset)
        {
            frame->setObjectName(QString::fromStdString(objectName()));
            // By property rather than by object name: object names here are
            // paths full of '.', '[' and ':', none of which a QSS id selector
            // takes. Only this frame carries the property, so the border does
            // not cascade onto the QLabels (also QFrames) inside it.
            frame->setProperty("inset", true);
            frame->setStyleSheet(
                "QFrame[inset=\"true\"] { border: 1px solid palette(mid); border-radius: 4px; }");
            form->setContentsMargins(8, 6, 8, 6);
        }
        else
        {
            form->setContentsMargins(0, 0, 0, 0);
        }

        const std::uint64_t id = schema.getProto().getId();
        for (const auto& field : inCodeOrder(schema.getNonUnionFields()))
        {
            const std::string name = field.getProto().getName().cStr();
            auto editor = makeEditor(field.getType(), &field, childPath(path_, name), changed_);

            std::string type_name = capnpTypeName(field.getType());
            if (field.getProto().isGroup())
            {
                type_name = isUnionGroup(field.getType().asStruct()) ? "union" : "group";
            }

            form->addRow(makeLabel(name, type_name, pub_sub::member_doc(id, field.getIndex())),
                         editor->widget());
            children_.emplace_back(name, std::move(editor));
        }
    }

    QWidget* widget() override { return frame_; }

    json value() const override
    {
        json out = json::object();
        for (const auto& [name, editor] : children_)
        {
            if (editor->settable())
            {
                out[name] = editor->value();
            }
        }
        return out;
    }

    void apply(const json& value) override
    {
        if (!value.is_object())
        {
            return;
        }
        for (const auto& [name, editor] : children_)
        {
            if (const auto it = value.find(name); it != value.end())
            {
                editor->apply(*it);
            }
        }
    }

    void visit(const std::function<void(FieldEditor&)>& visitor) override
    {
        visitor(*this);
        for (const auto& [name, editor] : children_)
        {
            editor->visit(visitor);
        }
    }

  private:
    QWidget* frame_ = nullptr;
    std::vector<std::pair<std::string, std::unique_ptr<FieldEditor>>> children_;
};

// A choice of arm, and the chosen arm's editor. Every arm's editor exists, so
// switching back and forth keeps what was typed in each.
class UnionEditor : public FieldEditor
{
  public:
    UnionEditor(std::string path, Changed changed, capnp::StructSchema schema) :
        FieldEditor(std::move(path), std::move(changed))
    {
        container_ = new QWidget();
        auto* layout = new QVBoxLayout(container_);
        layout->setContentsMargins(0, 0, 0, 0);

        combo_ = new QComboBox();
        combo_->setObjectName(QString::fromStdString(objectName() + ":arm"));
        stack_ = new QStackedWidget();
        layout->addWidget(combo_);
        layout->addWidget(stack_);

        const std::uint64_t id = schema.getProto().getId();
        for (const auto& field : inCodeOrder(schema.getFields()))
        {
            const std::string name = field.getProto().getName().cStr();
            auto editor = makeEditor(field.getType(), &field, childPath(path_, name), changed_);
            combo_->addItem(QString::fromStdString(name));

            std::string_view doc = pub_sub::member_doc(id, field.getIndex());
            QString tip = QString::fromStdString(capnpTypeName(field.getType()));
            if (!doc.empty())
            {
                tip += "\n" + QString::fromUtf8(doc.data(), static_cast<qsizetype>(doc.size()));
            }
            combo_->setItemData(combo_->count() - 1, tip, Qt::ToolTipRole);

            // Every arm starts at its type's default, not blank: the message
            // only ever carries one arm, so the others would otherwise sit
            // empty and be refused the moment someone switched to one.
            editor->apply(defaultFor(field.getType()));

            stack_->addWidget(editor->widget());
            arms_.emplace_back(name, std::move(editor));
        }

        track(QObject::connect(combo_, &QComboBox::currentIndexChanged,
                               [this](int index)
                               {
                                   stack_->setCurrentIndex(index);
                                   notifyChanged();
                               }));
    }

    QWidget* widget() override { return container_; }

    json value() const override
    {
        const int index = combo_->currentIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= arms_.size())
        {
            return json::object();
        }
        const auto& [name, editor] = arms_[static_cast<std::size_t>(index)];
        return json{{name, editor->value()}};
    }

    void apply(const json& value) override
    {
        if (!value.is_object() || value.size() != 1)
        {
            return;
        }
        // A named iterator, not `*value.items().begin()`: that dereference
        // returns a reference into the temporary iterator, dangling by the next
        // line.
        const auto first = value.begin();
        const std::string name = first.key();
        const json& arm_value = first.value();
        for (std::size_t i = 0; i < arms_.size(); ++i)
        {
            if (arms_[i].first == name)
            {
                const QSignalBlocker blocker(combo_);
                combo_->setCurrentIndex(static_cast<int>(i));
                stack_->setCurrentIndex(static_cast<int>(i));
                arms_[i].second->apply(arm_value);
                return;
            }
        }
    }

    // The ACTIVE arm only. The others are not part of the request, so nothing
    // typed into them can make it invalid -- and visiting them would report a
    // half-edited arm someone switched away from as a problem with what is
    // about to be sent.
    void visit(const std::function<void(FieldEditor&)>& visitor) override
    {
        visitor(*this);
        const int index = combo_->currentIndex();
        if (index >= 0 && static_cast<std::size_t>(index) < arms_.size())
        {
            arms_[static_cast<std::size_t>(index)].second->visit(visitor);
        }
    }

  private:
    QWidget* container_ = nullptr;
    QComboBox* combo_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    std::vector<std::pair<std::string, std::unique_ptr<FieldEditor>>> arms_;
};

// Rows with a remove button each, and an add button -- or exactly N rows and
// neither, when the schema's $fixedLength says how many there always are.
class ListEditor : public FieldEditor
{
  public:
    ListEditor(std::string path, Changed changed, capnp::ListSchema schema,
               std::optional<std::uint32_t> fixed_length) :
        FieldEditor(std::move(path), std::move(changed)),
        element_type_(schema.getElementType()),
        fixed_length_(fixed_length)
    {
        container_ = new QWidget();
        auto* layout = new QVBoxLayout(container_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        rows_ = new QWidget();
        rows_layout_ = new QVBoxLayout(rows_);
        rows_layout_->setContentsMargins(0, 0, 0, 0);
        rows_layout_->setSpacing(4);
        layout->addWidget(rows_);

        empty_ = new QLabel("(empty)");
        empty_->setStyleSheet("color: palette(mid);");
        layout->addWidget(empty_);

        if (!fixed_length_)
        {
            auto* add = new QPushButton("+ Add");
            add->setObjectName(QString::fromStdString(objectName() + ":add"));
            add->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            track(QObject::connect(add, &QPushButton::clicked,
                                   [this]()
                                   {
                                       json items = value();
                                       items.push_back(defaultFor(element_type_));
                                       apply(items);
                                       notifyChanged();
                                   }));
            layout->addWidget(add);
        }

        apply(json::array());
    }

    QWidget* widget() override { return container_; }

    json value() const override
    {
        json out = json::array();
        for (const auto& row : rows_list_)
        {
            out.push_back(row.editor->value());
        }
        return out;
    }

    void apply(const json& value) override
    {
        json items = value.is_array() ? value : json::array();
        if (fixed_length_)
        {
            while (items.size() < *fixed_length_)
            {
                items.push_back(defaultFor(element_type_));
            }
            while (items.size() > *fixed_length_)
            {
                items.erase(items.size() - 1);
            }
        }

        // Rebuilt, not patched: element paths are positional, so removing row 1
        // renames every row after it, and object names have to follow.
        // Detached as well as deferred: a row's widgets carry the same object
        // names as their replacements, and must not be found by name in the
        // turn before they are deleted.
        for (auto& row : rows_list_)
        {
            row.widget->hide();
            row.widget->setParent(nullptr);
            row.widget->deleteLater();
        }
        rows_list_.clear();

        for (std::size_t i = 0; i < items.size(); ++i)
        {
            addRow(i, items[i]);
        }
        empty_->setVisible(rows_list_.empty());
    }

    void visit(const std::function<void(FieldEditor&)>& visitor) override
    {
        visitor(*this);
        for (const auto& row : rows_list_)
        {
            row.editor->visit(visitor);
        }
    }

  private:
    struct Row
    {
        QWidget* widget = nullptr;
        std::unique_ptr<FieldEditor> editor;
    };

    void addRow(std::size_t index, const json& item)
    {
        const std::string element_path = path_ + "[" + std::to_string(index) + "]";

        auto* row = new QWidget(rows_);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* number = new QLabel(QString("%1").arg(index));
        number->setStyleSheet("color: palette(mid); font-size: 11px;");
        number->setMinimumWidth(18);
        layout->addWidget(number, 0, Qt::AlignTop);

        auto editor = makeEditor(element_type_, nullptr, element_path, changed_);
        editor->apply(item);
        layout->addWidget(editor->widget(), 1);

        if (!fixed_length_)
        {
            auto* remove = new QPushButton("✕");
            remove->setObjectName(QString::fromStdString("field:" + element_path + ":remove"));
            remove->setFixedWidth(28);
            remove->setToolTip("Remove this element");
            track(QObject::connect(remove, &QPushButton::clicked,
                                   [this, index]()
                                   {
                                       json items = value();
                                       if (index < items.size())
                                       {
                                           items.erase(index);
                                       }
                                       apply(items);
                                       notifyChanged();
                                   }));
            layout->addWidget(remove, 0, Qt::AlignTop);
        }

        rows_layout_->addWidget(row);
        rows_list_.push_back(Row{row, std::move(editor)});
    }

    capnp::Type element_type_;
    std::optional<std::uint32_t> fixed_length_;
    QWidget* container_ = nullptr;
    QWidget* rows_ = nullptr;
    QVBoxLayout* rows_layout_ = nullptr;
    QLabel* empty_ = nullptr;
    std::vector<Row> rows_list_;
};

}  // namespace

std::unique_ptr<FieldEditor> makeEditor(const capnp::Type& type,
                                        const capnp::StructSchema::Field* field,
                                        const std::string& path, const Changed& changed)
{
    using W = capnp::schema::Type::Which;
    const QString type_name = QString::fromStdString(capnpTypeName(type));

    switch (type.which())
    {
        case W::STRUCT:
        {
            const auto schema = type.asStruct();
            if (field != nullptr && field->getProto().isGroup() && isUnionGroup(schema))
            {
                return std::make_unique<UnionEditor>(path, changed, schema);
            }
            return std::make_unique<StructEditor>(path, changed, schema, true);
        }
        case W::LIST:
        {
            const auto fixed = field != nullptr ? pub_sub::fixedListLength(*field) : std::nullopt;
            return std::make_unique<ListEditor>(path, changed, type.asList(), fixed);
        }
        case W::BOOL:
            return std::make_unique<BoolEditor>(path, changed);
        case W::ENUM:
            return std::make_unique<EnumEditor>(path, changed, type.asEnum());
        case W::TEXT:
            return std::make_unique<TextEditor>(path, changed);
        case W::DATA:
            return std::make_unique<DataEditor>(path, changed);
        case W::INT8:
        case W::INT16:
        case W::INT32:
        case W::INT64:
        case W::UINT8:
        case W::UINT16:
        case W::UINT32:
        case W::UINT64:
            return std::make_unique<IntegerEditor>(path, changed, type_name);
        case W::FLOAT32:
        case W::FLOAT64:
            return std::make_unique<FloatEditor>(path, changed, type_name);
        case W::VOID:
            return std::make_unique<VoidEditor>(path, changed);
        case W::INTERFACE:
        case W::ANY_POINTER:
            return std::make_unique<UnsupportedEditor>(path, changed);
    }
    return std::make_unique<UnsupportedEditor>(path, changed);
}

std::string capnpTypeName(const capnp::Type& type)
{
    using W = capnp::schema::Type::Which;
    switch (type.which())
    {
        case W::VOID:        return "Void";
        case W::BOOL:        return "Bool";
        case W::INT8:        return "Int8";
        case W::INT16:       return "Int16";
        case W::INT32:       return "Int32";
        case W::INT64:       return "Int64";
        case W::UINT8:       return "UInt8";
        case W::UINT16:      return "UInt16";
        case W::UINT32:      return "UInt32";
        case W::UINT64:      return "UInt64";
        case W::FLOAT32:     return "Float32";
        case W::FLOAT64:     return "Float64";
        case W::TEXT:        return "Text";
        case W::DATA:        return "Data";
        case W::LIST:        return "List(" + capnpTypeName(type.asList().getElementType()) + ")";
        case W::ENUM:        return type.asEnum().getShortDisplayName().cStr();
        case W::STRUCT:      return type.asStruct().getShortDisplayName().cStr();
        case W::INTERFACE:   return "Interface";
        case W::ANY_POINTER: return "AnyPointer";
    }
    return "Unknown";
}

// --------------------------------------------------------------- SchemaForm

SchemaForm::SchemaForm(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addStretch(1);
}

SchemaForm::~SchemaForm() = default;

void SchemaForm::setSchema(std::optional<capnp::StructSchema> schema)
{
    if (container_ != nullptr)
    {
        container_->hide();
        container_->setParent(nullptr);
        container_->deleteLater();
        container_ = nullptr;
    }
    root_.reset();
    schema_ = schema;

    if (schema_)
    {
        root_ = std::make_unique<StructEditor>(
            "", [this]() { if (!applying_) { revalidate(); emit edited(); } }, *schema_, false);
        container_ = root_->widget();
        container_->setParent(this);
        static_cast<QVBoxLayout*>(layout())->insertWidget(0, container_);
        container_->show();

        applying_ = true;
        root_->apply(defaults());
        applying_ = false;
    }

    revalidate();
    emit edited();
}

json SchemaForm::defaults() const
{
    if (!schema_)
    {
        return json::object();
    }
    // Read back from an untouched message, so a default the schema declares
    // (`count @1 :UInt16 = 5`) is what the form shows -- not a zero the form
    // made up.
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(*schema_);
    return pub_sub::capnpToJson(root.asReader(), kAllDataAsHex);
}

bool SchemaForm::hasSchema() const
{
    return schema_.has_value();
}

json SchemaForm::value() const
{
    return root_ ? root_->value() : json::object();
}

bool SchemaForm::setValue(const json& fields, std::vector<std::string>& errors)
{
    if (!schema_ || !root_)
    {
        errors.push_back("no service is selected, so there is no form to fill in.");
        return false;
    }
    if (!fields.is_object())
    {
        errors.push_back("fields must be a JSON object.");
        return false;
    }

    // Validated as the form WOULD be afterwards, before anything is touched.
    // A merged candidate rather than `fields` alone: a request is valid or not
    // as a whole, and a partial update that names only good fields can still
    // leave an existing bad one in place -- which is fine, and not this call's
    // error, so only problems in the named fields are reported.
    json candidate = value();
    for (const auto& [key, item] : fields.items())
    {
        candidate[key] = item;
    }

    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(*schema_);
    std::vector<std::string> all;
    pub_sub::jsonToCapnp(candidate, root, all);

    const std::size_t before = errors.size();
    for (const std::string& error : all)
    {
        for (const auto& [key, item] : fields.items())
        {
            if (error.starts_with(key + ":") || error.starts_with(key + ".") ||
                error.starts_with(key + "["))
            {
                errors.push_back(error);
                break;
            }
        }
    }
    if (errors.size() != before)
    {
        return false;
    }

    applying_ = true;
    root_->apply(fields);
    applying_ = false;

    revalidate();
    emit edited();
    return true;
}

void SchemaForm::resetToDefaults()
{
    if (!root_)
    {
        return;
    }
    applying_ = true;
    root_->apply(defaults());
    applying_ = false;
    revalidate();
    emit edited();
}

bool SchemaForm::isValid() const
{
    return valid_;
}

const std::vector<std::string>& SchemaForm::problems() const
{
    return problems_;
}

void SchemaForm::revalidate()
{
    problems_.clear();

    if (root_)
    {
        std::vector<FieldEditor*> editors;
        root_->visit([&editors](FieldEditor& editor) { editors.push_back(&editor); });

        std::map<std::string, std::string> parse_problems;
        for (FieldEditor* editor : editors)
        {
            editor->showProblem(std::nullopt);
            if (const auto problem = editor->parseProblem())
            {
                parse_problems.emplace(editor->path(), *problem);
                editor->showProblem(problem);
                problems_.push_back(editor->path() + ": " + *problem);
            }
        }

        // The send path's rules, applied to what the form would send.
        capnp::MallocMessageBuilder message;
        auto root = message.initRoot<capnp::DynamicStruct>(*schema_);
        std::vector<std::string> errors;
        pub_sub::jsonToCapnp(root_->value(), root, errors);

        for (const std::string& error : errors)
        {
            // The editor whose path is the longest prefix of the message --
            // `inner.count: ...` belongs to inner.count, not to inner.
            FieldEditor* owner = nullptr;
            for (FieldEditor* editor : editors)
            {
                const std::string& path = editor->path();
                if (!path.empty() && error.starts_with(path + ": ") &&
                    (owner == nullptr || path.size() > owner->path().size()))
                {
                    owner = editor;
                }
            }

            if (owner != nullptr && parse_problems.count(owner->path()) != 0)
            {
                continue;  // Already reported, in better words.
            }
            if (owner != nullptr)
            {
                owner->showProblem(error.substr(owner->path().size() + 2));
            }
            problems_.push_back(error);
        }
    }

    const bool valid = problems_.empty();
    if (valid != valid_)
    {
        valid_ = valid;
        emit validityChanged(valid_);
    }
}

}  // namespace switchboard
