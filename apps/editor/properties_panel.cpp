#include "editor/properties_panel.h"
#include "dashboard/widget_registry.h"
#include "editor/canvas.h"
#include "editor/selection_frame.h"
#include "editor/editor_constants.h"
#include "qt_helpers/reflected_form.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QStackedWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QFrame>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QGroupBox>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>

#include <functional>

#include "reflection/reflection.h"
#include "helpers/color.h"
#include "spdlog/spdlog.h"

#include <string>
#include <vector>

PropertiesPanel::PropertiesPanel(QWidget* parent):
  QWidget(parent),
  selected_(nullptr),
  stack_(new QStackedWidget(this)),
  currentPage_(nullptr),
  windowPage_(nullptr),
  winNameEdit_(nullptr),
  winWidthSpin_(nullptr),
  winHeightSpin_(nullptr),
  winBgColorEdit_(nullptr),
  canvas_(nullptr),
  isSyncing_(false)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Says what is being edited, not just that something is. The panel used to
    // be headed "Properties" whatever was selected, so with two widgets of the
    // same type on the canvas there was nothing on screen to tell you which
    // one's values you were looking at.
    heading_ = new QLabel(this);
    heading_->setStyleSheet("QLabel { font-weight: 700; font-size: 13px; }");
    subheading_ = new QLabel(this);
    subheading_->setStyleSheet("QLabel { color: palette(mid); font-size: 11px; }");
    subheading_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(heading_);
    layout->addWidget(subheading_);
    layout->addWidget(stack_);
    layout->addStretch();
    setLayout(layout);

    showHeading(nullptr);

    // Default page
    buildWindowPage();
    stack_->addWidget(windowPage_);
    // Initialize with defaults until a canvas is attached
    winWidthSpin_->setValue(editor_defaults::kDefaultCanvasWidth);
    winHeightSpin_->setValue(editor_defaults::kDefaultCanvasHeight);
    stack_->setCurrentWidget(windowPage_);
}

// The heading for the current selection: the widget's friendly name over the
// selector that addresses it. Nothing selected means the window's own
// properties, which is what the panel falls back to.
void PropertiesPanel::showHeading(SelectionFrame* frame)
{
    if (frame == nullptr)
    {
        heading_->setText("Window");
        subheading_->setText("No widget selected");
        return;
    }

    const widget_descriptor_t* descriptor = widgetDescriptor(frame->type());
    const std::string_view friendly_name =
        descriptor != nullptr ? descriptor->friendly_name : reflection::enum_to_string(frame->type());
    const QString friendly =
        QString::fromUtf8(friendly_name.data(), static_cast<qsizetype>(friendly_name.size()));

    heading_->setText(friendly);
    QString where = frame->objectName();
    if (SelectionFrame* stack = frame->containerFrame())
    {
        if (const auto slot = stack->locateChild(frame))
        {
            where += QString(" \u00b7 on %1 / %2")
                         .arg(stack->objectName(), QString::fromStdString(stack->pageName(slot->first)));
        }
    }
    subheading_->setText(where);
}

namespace
{

// Runs a page edit after the current event returns. Every page edit re-emits the
// selection, which rebuilds this panel's form -- including the button or field
// whose signal is running right now. Deleting it from inside its own handler is
// a use-after-free, so the edit waits for the handler to finish.
void deferToCanvas(Canvas* canvas, std::function<void()> action)
{
    if (canvas == nullptr) return;
    QMetaObject::invokeMethod(canvas, std::move(action), Qt::QueuedConnection);
}

// The pages of a selected page_stack: which one the canvas previews, and the
// structure -- add, remove, reorder, rename, in or out of the cycle.
QWidget* buildPagesEditor(QWidget* parent, Canvas* canvas, SelectionFrame* stack)
{
    auto* box = new QGroupBox("Pages", parent);
    box->setObjectName("pages:editor");
    auto* layout = new QVBoxLayout(box);

    auto* list = new QListWidget(box);
    list->setObjectName("pages:list");
    list->setMaximumHeight(120);
    for (std::size_t i = 0; i < stack->pageCount(); ++i)
    {
        QString label = QString::fromStdString(stack->pageName(i));
        if (!stack->pageInCycle(i))
        {
            label += "  (not in cycle)";
        }
        list->addItem(label);
    }
    list->setCurrentRow(static_cast<int>(stack->shownPage()));
    layout->addWidget(list);

    auto* buttons = new QHBoxLayout();
    const auto makeButton = [box, buttons](const char* text, const char* name, const char* tip)
    {
        auto* button = new QPushButton(text, box);
        button->setObjectName(name);
        button->setToolTip(tip);
        buttons->addWidget(button);
        return button;
    };
    QPushButton* add = makeButton("Add", "pages:add", "Add an empty page at the end");
    QPushButton* remove = makeButton("Remove", "pages:remove", "Remove the selected page and its widgets");
    QPushButton* up = makeButton("Up", "pages:up", "Move the selected page earlier in the cycle");
    QPushButton* down = makeButton("Down", "pages:down", "Move the selected page later in the cycle");
    layout->addLayout(buttons);

    auto* form = new QFormLayout();
    auto* name = new QLineEdit(box);
    name->setObjectName("pages:name");
    auto* inCycle = new QCheckBox("Stops here on next/prev", box);
    inCycle->setObjectName("pages:in_cycle");
    form->addRow("Name", name);
    form->addRow("In cycle", inCycle);
    layout->addLayout(form);

    auto* hint = new QLabel("Double-click the stack on the canvas to edit this page's widgets. "
                            "PageUp/PageDown change the page shown.", box);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: palette(mid); font-size: 11px;");
    layout->addWidget(hint);

    const QPointer<SelectionFrame> guarded(stack);
    const auto syncRow = [guarded, list, name, inCycle, remove, up, down]()
    {
        const int row = list->currentRow();
        const bool valid = guarded && row >= 0 && static_cast<std::size_t>(row) < guarded->pageCount();
        const QSignalBlocker b1(name);
        const QSignalBlocker b2(inCycle);
        name->setText(valid ? QString::fromStdString(guarded->pageName(static_cast<std::size_t>(row))) : QString());
        inCycle->setChecked(valid && guarded->pageInCycle(static_cast<std::size_t>(row)));
        remove->setEnabled(valid && guarded->pageCount() > 1);
        up->setEnabled(valid && row > 0);
        down->setEnabled(valid && static_cast<std::size_t>(row) + 1 < guarded->pageCount());
    };
    syncRow();

    QObject::connect(list, &QListWidget::currentRowChanged, box, [canvas, guarded, syncRow](int row)
    {
        if (guarded && row >= 0)
        {
            canvas->showPage(guarded, static_cast<std::size_t>(row));
        }
        syncRow();
    });
    QObject::connect(add, &QPushButton::clicked, box, [canvas, guarded]()
    {
        deferToCanvas(canvas, [canvas, guarded]() { if (guarded) canvas->addPage(guarded); });
    });
    QObject::connect(remove, &QPushButton::clicked, box, [canvas, guarded, list]()
    {
        const int row = list->currentRow();
        deferToCanvas(canvas, [canvas, guarded, row]()
        {
            if (guarded && row >= 0) canvas->removePage(guarded, static_cast<std::size_t>(row));
        });
    });
    const auto move = [canvas, guarded, list](int delta)
    {
        const int row = list->currentRow();
        deferToCanvas(canvas, [canvas, guarded, row, delta]()
        {
            if (guarded && row >= 0 && row + delta >= 0)
            {
                canvas->movePage(guarded, static_cast<std::size_t>(row), static_cast<std::size_t>(row + delta));
            }
        });
    };
    QObject::connect(up, &QPushButton::clicked, box, [move]() { move(-1); });
    QObject::connect(down, &QPushButton::clicked, box, [move]() { move(+1); });
    QObject::connect(name, &QLineEdit::editingFinished, box, [canvas, guarded, list, name]()
    {
        const int row = list->currentRow();
        const std::string text = name->text().trimmed().toStdString();
        if (!guarded || row < 0 || text.empty() || text == guarded->pageName(static_cast<std::size_t>(row)))
        {
            return;
        }
        deferToCanvas(canvas, [canvas, guarded, row, text]()
        {
            if (guarded) canvas->renamePage(guarded, static_cast<std::size_t>(row), text);
        });
    });
    QObject::connect(inCycle, &QCheckBox::toggled, box, [canvas, guarded, list](bool on)
    {
        const int row = list->currentRow();
        deferToCanvas(canvas, [canvas, guarded, row, on]()
        {
            if (guarded && row >= 0) canvas->setPageInCycle(guarded, static_cast<std::size_t>(row), on);
        });
    });
    return box;
}

// For a widget on a page: which page it is on, and a way to move it.
QWidget* buildPagePicker(QWidget* parent, Canvas* canvas, SelectionFrame* child, SelectionFrame* stack)
{
    auto* row = new QWidget(parent);
    auto* form = new QFormLayout(row);
    form->setContentsMargins(10, 4, 10, 4);
    auto* combo = new QComboBox(row);
    combo->setObjectName("page:move_to");
    for (std::size_t i = 0; i < stack->pageCount(); ++i)
    {
        combo->addItem(QString::fromStdString(stack->pageName(i)));
    }
    if (const auto slot = stack->locateChild(child))
    {
        combo->setCurrentIndex(static_cast<int>(slot->first));
    }
    combo->setToolTip("The page of " + stack->objectName() + " this widget is on");
    form->addRow("Page", combo);

    const QPointer<SelectionFrame> guarded(child);
    QObject::connect(combo, &QComboBox::currentIndexChanged, row, [canvas, guarded](int index)
    {
        deferToCanvas(canvas, [canvas, guarded, index]()
        {
            if (guarded && index >= 0) canvas->moveToPage(guarded, static_cast<std::size_t>(index));
        });
    });
    return row;
}

}  // namespace

void PropertiesPanel::setCanvas(Canvas* canvas)
{
    canvas_ = canvas;
    if (canvas_ && windowPage_)
    {
        syncFromCanvas();
    }
}

namespace
{
    // The copy the form edits, for as long as the form lives. Applied whole on
    // Apply; seeded from the frame's stored config, not the live widget's,
    // which holds the clamped copy -- seeding from that would write the clamp
    // back on every Apply.
    template <typename Config>
    struct WorkingCopy
    {
        Config config;
    };

    template <typename Config>
    QWidget* buildFormFromConfig(PropertiesPanel* panel, const Config& cfg)
    {
        auto* page = new QWidget(panel);
        auto* vbox = new QVBoxLayout(page);
        vbox->setContentsMargins(0, 0, 0, 0);
        vbox->setSpacing(0);

        auto* working = new WorkingCopy<Config>{cfg};
        auto* scroll = new QScrollArea(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* form = new qt_helpers::ReflectedForm(working->config, scroll);
        // After the form's editors are gone: QWidget's destructor deletes its
        // children before destroyed() is emitted.
        QObject::connect(form, &QObject::destroyed, [working]() { delete working; });
        scroll->setWidget(form);
        vbox->addWidget(scroll, 1);

        // Why Apply is unavailable, said next to Apply. The field itself turns
        // red and carries the reason as a tooltip; this is the aggregate.
        auto* problemsLabel = new QLabel(page);
        problemsLabel->setObjectName("field_problems");
        problemsLabel->setWordWrap(true);
        problemsLabel->setStyleSheet("color: #E74C3C; font-size: 11px;");
        problemsLabel->hide();
        vbox->addWidget(problemsLabel, 0);

        auto* applyBtn = new QPushButton("Apply", page);
        applyBtn->setObjectName("apply_button");
        applyBtn->setMinimumHeight(28);
        applyBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* bottom = new QHBoxLayout();
        bottom->setContentsMargins(8, 8, 8, 8);
        bottom->addWidget(applyBtn);
        bottom->setSizeConstraint(QLayout::SetMinimumSize);
        vbox->addLayout(bottom, 0);

        const auto revalidate = [form, problemsLabel, applyBtn]()
        {
            const QStringList problems = form->problems();
            problemsLabel->setVisible(!problems.isEmpty());
            problemsLabel->setText(problems.join('\n'));
            applyBtn->setEnabled(problems.isEmpty());
            applyBtn->setToolTip(problems.isEmpty() ? QString()
                                                    : QString("Fix the highlighted field(s) first."));
        };
        QObject::connect(form, &qt_helpers::ReflectedForm::problemsChanged, page, revalidate);
        // Once for the config as loaded: a hand-edited file can already hold a
        // bad key, and the panel should say so on selection.
        revalidate();

        QObject::connect(applyBtn, &QPushButton::clicked, page, [panel, working]()
        {
            auto* frame = qobject_cast<SelectionFrame*>(panel->selected());
            if (frame == nullptr) return;
            // One history entry per Apply; discarded if nothing changed.
            const Canvas::EditTransaction tx(panel->canvas(), Canvas::EditSource::Widget);
            if (!frame->applyConfig(working->config))
            {
                SPDLOG_ERROR("Apply did nothing for '{}': the frame holds no configuration.",
                             frame->objectName().toStdString());
            }
        });
        return page;
    }
}

void PropertiesPanel::buildWindowPage()
{
    if (windowPage_) return;
    windowPage_ = new QWidget(this);
    auto* form = new QFormLayout(windowPage_);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setFormAlignment(Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setContentsMargins(8,8,8,8);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);


    // Named on the same rule as the per-widget form editors ("field:<path>"),
    // so the window's properties are addressable by name rather than by their
    // position in the layout -- which is what a test or the agent interface
    // would otherwise have to depend on.
    winNameEdit_ = new QLineEdit(windowPage_);
    winNameEdit_->setObjectName("window:name");

    winWidthSpin_ = new QSpinBox(windowPage_);
    winWidthSpin_->setRange(100u, 10000u);
    winWidthSpin_->setObjectName("window:width");

    winHeightSpin_ = new QSpinBox(windowPage_);
    winHeightSpin_->setRange(100u, 10000u);
    winHeightSpin_->setObjectName("window:height");

    winBgColorEdit_ = new QLineEdit(windowPage_);
    winBgColorEdit_->setPlaceholderText("#RRGGBB");
    winBgColorEdit_->setObjectName("window:background_color");

    winDisplayCombo_ = new QComboBox(windowPage_);
    winDisplayCombo_->setObjectName("window:display");
    for (const auto name : enum_names(display_role_t{}))
    {
        winDisplayCombo_->addItem(QString::fromStdString(std::string(name)));
    }
    winDisplayCombo_->setToolTip("The panel this window binds to on the target. Ignored on a desktop, "
                                 "where every window opens as an ordinary window.");

    winScaleCombo_ = new QComboBox(windowPage_);
    winScaleCombo_->setObjectName("window:scale");
    for (const auto name : enum_names(scale_mode_t{}))
    {
        winScaleCombo_->addItem(QString::fromStdString(std::string(name)));
    }
    winScaleCombo_->setToolTip("fit: scale the layout uniformly to the panel, letterboxing the rest. "
                               "none: leave scaling to the environment.");

    form->addRow("Name", winNameEdit_);
    form->addRow("Width", winWidthSpin_);
    form->addRow("Height", winHeightSpin_);
    form->addRow("Background Color", winBgColorEdit_);
    form->addRow("Display", winDisplayCombo_);
    form->addRow("Scale", winScaleCombo_);
    windowPage_->setLayout(form);

    // A choice is a whole edit, so it opens and closes its own history entry.
    for (QComboBox* combo : {winDisplayCombo_, winScaleCombo_})
    {
        connect(combo, &QComboBox::currentIndexChanged, this, [this](int)
        {
            applyWindowEdits();
            commitWindowEdits();
        });
    }

    connect(winNameEdit_, &QLineEdit::textEdited, this, [this]{ applyWindowEdits(); });
    connect(winWidthSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int){ applyWindowEdits(); });
    connect(winHeightSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int){ applyWindowEdits(); });
    connect(winBgColorEdit_, &QLineEdit::textEdited, this, [this]{ applyWindowEdits(); });

    // Close the history entry when the field is done, not on every keystroke.
    //
    // applyWindowEdits() opens one on the first change and beginEdit() collapses
    // the rest, so the pair is "one entry per edited field" rather than one per
    // character. editingFinished covers both Enter and focus loss, which is what
    // makes clicking away from the field commit it.
    for (QLineEdit* edit : {winNameEdit_, winBgColorEdit_})
    {
        connect(edit, &QLineEdit::editingFinished, this, [this]{ commitWindowEdits(); });
    }
    for (QSpinBox* spin : {winWidthSpin_, winHeightSpin_})
    {
        connect(spin, &QAbstractSpinBox::editingFinished, this, [this]{ commitWindowEdits(); });
    }
}

void PropertiesPanel::discardCurrentPage()
{
    if (currentPage_)
    {
        // Take it out of the stack before deleting so the stack never holds a
        // dangling entry. This used to leak a page into the stack on every
        // selection.
        stack_->removeWidget(currentPage_);

        // Destroyed now, not deleteLater'd. removeWidget() only takes the page
        // out of the layout -- it stays a child of the stack until it is really
        // deleted, and every field editor on it keeps its "field:<path>"
        // objectName. Anything doing findChild() before the event loop next runs
        // would then find the *old* page's editor and read the previously
        // selected widget's values. MainWindow::rebuildWidget destroys
        // synchronously for the same reason.
        //
        // Safe here because selection changes arrive from the canvas, never from
        // inside one of this page's own widgets.
        delete currentPage_;
        currentPage_ = nullptr;
    }
}

void PropertiesPanel::showPage(QWidget* page)
{
    currentPage_ = page;
    stack_->addWidget(page);
    stack_->setCurrentWidget(page);
}

void PropertiesPanel::showUnsupported(const QString& name)
{
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->addWidget(new QLabel(QString("%1 properties not yet supported").arg(name)));
    v->addStretch();
    showPage(page);
}

void PropertiesPanel::setSelectedWidget(QWidget* w)
{
    selected_ = w;

    // Always start from a clean page. The form's values come from this
    // selection's live config, so nothing from the previous one may survive.
    discardCurrentPage();
    showHeading(qobject_cast<SelectionFrame*>(w));

    if (!w)
    {
        // Show window properties when no widget selected
        if (windowPage_) stack_->setCurrentWidget(windowPage_);
        if (canvas_)
        {
            syncFromCanvas();
        }
        return;
    }

    // Unwrap SelectionFrame for UI classification
    QWidget* uiWidget = w;
    if (auto* frame = qobject_cast<SelectionFrame*>(uiWidget)) uiWidget = frame->child();

    // No child means the frame holds nothing to configure. Bail rather than
    // fall through to `uiWidget = w`, which fed a SelectionFrame* into a
    // static_cast to an unrelated widget type below.
    if (!uiWidget)
    {
        showUnsupported(w->metaObject()->className());
        return;
    }

    // Build the form from the frame's stored config, not from the live widget's.
    // They differ: the widget holds the clamped copy widget_factory built it
    // from, so a field the clamp moved would be shown as the clamped value and
    // then written back on Apply, quietly editing the user's config for them.
    QWidget* page = nullptr;
    if (auto* frame = qobject_cast<SelectionFrame*>(w))
    {
        std::visit(
            [&](const auto& cfg)
            {
                using cfg_t = std::decay_t<decltype(cfg)>;
                if constexpr (!std::is_same_v<cfg_t, std::monostate>)
                {
                    page = buildFormFromConfig<cfg_t>(this, cfg);
                }
            },
            frame->config());
    }

    if (page)
    {
        // A stack's pages, or a page widget's page, around the reflected form.
        if (auto* frame = qobject_cast<SelectionFrame*>(w); frame && canvas_)
        {
            if (auto* vbox = qobject_cast<QVBoxLayout*>(page->layout()))
            {
                if (frame->isContainer())
                {
                    vbox->insertWidget(1, buildPagesEditor(page, canvas_, frame));
                }
                else if (SelectionFrame* stack = frame->containerFrame())
                {
                    vbox->insertWidget(0, buildPagePicker(page, canvas_, frame, stack));
                }
            }
        }
        showPage(page);
        return;
    }

    // Other types unsupported for now
    showUnsupported(w->metaObject()->className());
    // leave as unsupported page
}

// Removed per reflect-first UI goal; wiring updates will follow later.

void PropertiesPanel::applyWindowEdits()
{
    if (isSyncing_) return; // avoid pushing during UI sync
    if (!canvas_) return;

    // These are document edits like any other, and they were the one mutation
    // path that never said so. The window's name, size and background changed
    // the canvas without opening a history entry, so they could not be undone --
    // and worse, the *next* edit's beginEdit() snapshotted the already-changed
    // state, baking them in permanently. The dirty flag noticed (isDirty
    // compares snapshots) but nothing emitted historyChanged, so the title bar
    // kept its old text until some unrelated edit refreshed it.
    //
    // beginEdit() collapses repeated calls, so this is the *first* keystroke
    // capturing the pre-edit state; commitWindowEdits() closes it. Tagged
    // Window so that an edit from anywhere else closes it rather than merging
    // with it -- editingFinished needs a focus change, and the agent interface
    // never moves focus.
    canvas_->beginEdit(Canvas::EditSource::Window);

    // The name is round-tripped through the canvas into the saved YAML. It used
    // to be collected here and never read, while save hardcoded its own.
    canvas_->setWindowName(winNameEdit_->text().toStdString());

    if (const auto role = reflection::enum_traits<display_role_t>::try_from_string(
            winDisplayCombo_->currentText().toStdString()))
    {
        canvas_->setDisplay(*role);
    }
    if (const auto scale = reflection::enum_traits<scale_mode_t>::try_from_string(
            winScaleCombo_->currentText().toStdString()))
    {
        canvas_->setScale(*scale);
    }

    // Only apply a colour that is actually a colour. Half-typed text arrives
    // here on every keystroke -- "#ff00" on the way to "#ff0000" -- and an
    // unparseable value silently becomes the fallback, so without this the
    // preview flickered through black as you typed. Same check the loader
    // applies (see validate_app_config), so the editor and the file agree on
    // what a colour is.
    const QString bg = winBgColorEdit_->text();
    if (!bg.isEmpty() && helpers::Color::isValidFormat(bg.toStdString()))
    {
        canvas_->setBackgroundColor(bg);
    }
    if (winWidthSpin_->value() > 0 && winHeightSpin_->value() > 0) {
        // Resize the central canvas viewport for preview purposes
        canvas_->resize(winWidthSpin_->value(), winHeightSpin_->value());
    }
}

void PropertiesPanel::commitWindowEdits()
{
    if (isSyncing_ || !canvas_)
    {
        return;
    }
    // No-op when nothing is open, and commitEdit() itself discards an entry
    // whose before and after match -- so tabbing through the fields without
    // changing anything adds nothing to the history.
    canvas_->commitEdit();
}

void PropertiesPanel::syncFromCanvas()
{
    if (!canvas_ || !windowPage_) return;
    const QSignalBlocker b1(winNameEdit_);
    const QSignalBlocker b2(winWidthSpin_);
    const QSignalBlocker b3(winHeightSpin_);
    const QSignalBlocker b4(winBgColorEdit_);
    const QSignalBlocker b5(winDisplayCombo_);
    const QSignalBlocker b6(winScaleCombo_);
    isSyncing_ = true;
    winNameEdit_->setText(QString::fromStdString(canvas_->windowName()));
    winWidthSpin_->setValue(canvas_->width());
    winHeightSpin_->setValue(canvas_->height());
    winBgColorEdit_->setText(canvas_->getBackgroundColorHex());
    winDisplayCombo_->setCurrentText(QString::fromStdString(std::string(reflection::enum_to_string(canvas_->display()))));
    winScaleCombo_->setCurrentText(QString::fromStdString(std::string(reflection::enum_to_string(canvas_->scale()))));
    isSyncing_ = false;
}

#include "editor/moc_properties_panel.cpp"

