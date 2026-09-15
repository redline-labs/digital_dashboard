#include "scope/settings_dialog.h"

#include "mbtiles/archive.h"
#include "mbtiles/error.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace scope
{
namespace
{

constexpr int kNameColumn = 0;
constexpr int kPathColumn = 1;
constexpr int kStatusColumn = 2;

}  // namespace

SettingsDialog::SettingsDialog(scope_settings_t settings, QWidget* parent)
    : QDialog(parent), settings_(std::move(settings))
{
    setObjectName("settings_dialog");
    setWindowTitle(tr("Settings"));

    auto* layout = new QVBoxLayout(this);

    auto* caption = new QLabel(
        tr("Map archives on this machine. A map panel refers to one by NAME, so a "
           "workspace stays portable — the path lives here and nowhere else."),
        this);
    caption->setObjectName("settings_caption");
    caption->setWordWrap(true);
    layout->addWidget(caption);

    table_ = new QTableWidget(0, 3, this);
    table_->setObjectName("settings_tilesets");
    table_->setHorizontalHeaderLabels({tr("Name"), tr("Path"), tr("Status")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(kPathColumn, QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table_, 1);

    for (const scope_tileset_t& tileset : settings_.tilesets)
    {
        addRow(tileset);
    }

    auto* buttons = new QHBoxLayout();

    auto* add = new QPushButton(tr("Add"), this);
    add->setObjectName("settings_add");
    connect(add, &QPushButton::clicked, this, [this]() {
        addRow({});
        table_->setCurrentCell(table_->rowCount() - 1, kNameColumn);
    });
    buttons->addWidget(add);

    remove_ = new QPushButton(tr("Remove"), this);
    remove_->setObjectName("settings_remove");
    connect(remove_, &QPushButton::clicked, this, [this]() {
        const int row = table_->currentRow();
        if (row >= 0)
        {
            table_->removeRow(row);
        }
    });
    buttons->addWidget(remove_);

    auto* browse = new QPushButton(tr("Browse…"), this);
    browse->setObjectName("settings_browse");
    connect(browse, &QPushButton::clicked, this, [this]() {
        const int row = table_->currentRow();
        if (row >= 0)
        {
            browseForRow(row);
        }
    });
    buttons->addWidget(browse);

    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->setObjectName("settings_buttons");
    box->button(QDialogButtonBox::Ok)->setObjectName("settings_ok");
    box->button(QDialogButtonBox::Cancel)->setObjectName("settings_cancel");
    connect(box, &QDialogButtonBox::accepted, this, [this]() {
        collect();
        accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    // Re-probe whenever a path is edited by hand, not only via Browse. A status
    // describing the archive that USED to be on that row is worse than an empty
    // one, because it reads as confirmation.
    connect(table_, &QTableWidget::cellChanged, this, [this](int row, int column) {
        if (column == kPathColumn)
        {
            refreshStatus(row);
        }
    });

    resize(760, 320);
}

void SettingsDialog::addRow(const scope_tileset_t& tileset)
{
    const int row = table_->rowCount();
    table_->insertRow(row);
    table_->setItem(row, kNameColumn, new QTableWidgetItem(QString::fromStdString(tileset.name)));
    table_->setItem(row, kPathColumn, new QTableWidgetItem(QString::fromStdString(tileset.path)));

    auto* status = new QTableWidgetItem();
    // Read-only: it is what the archive says, not something to type.
    status->setFlags(status->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, kStatusColumn, status);

    refreshStatus(row);
}

void SettingsDialog::browseForRow(int row)
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select a map archive"), QString(), tr("Map archives (*.mbtiles);;All files (*)"));
    if (path.isEmpty())
    {
        return;
    }

    table_->item(row, kPathColumn)->setText(path);

    // A name is what everything downstream uses, and an unnamed row is
    // unreachable. The file's stem is nearly always what someone would have
    // typed anyway, and it is only filled in when the field is still empty.
    QTableWidgetItem* name = table_->item(row, kNameColumn);
    if (name->text().isEmpty())
    {
        name->setText(QFileInfo(path).completeBaseName());
    }
}

void SettingsDialog::refreshStatus(int row)
{
    QTableWidgetItem* status = table_->item(row, kStatusColumn);
    if (status == nullptr)
    {
        return;
    }

    const QString path = table_->item(row, kPathColumn)->text();
    if (path.isEmpty())
    {
        status->setText(tr("no path"));
        return;
    }

    auto archive = mbtiles::Archive::open(path.toStdString());
    if (!archive)
    {
        // The actual reason, verbatim. "Cannot open" is the same sentence for a
        // typo, a permissions problem and a file that is not an archive at all,
        // and those are three different fixes.
        status->setText(QString::fromStdString(mbtiles::to_string(archive.error())));
        return;
    }

    const mbtiles::Metadata& meta = archive->metadata();
    status->setText(tr("z%1–%2  %3")
                        .arg(meta.minzoom)
                        .arg(meta.maxzoom)
                        .arg(QString::fromStdString(meta.format)));
}

void SettingsDialog::collect()
{
    settings_.tilesets.clear();
    for (int row = 0; row < table_->rowCount(); ++row)
    {
        scope_tileset_t tileset;
        tileset.name = table_->item(row, kNameColumn)->text().trimmed().toStdString();
        tileset.path = table_->item(row, kPathColumn)->text().trimmed().toStdString();

        // A row with neither is one someone added and abandoned; dropping it is
        // what they meant. A row with one of the two is kept, so that
        // checkTilesets() can say what is wrong with it rather than the entry
        // silently disappearing.
        if (tileset.name.empty() && tileset.path.empty())
        {
            continue;
        }
        settings_.tilesets.push_back(std::move(tileset));
    }
}

}  // namespace scope

#include "scope/moc_settings_dialog.cpp"
