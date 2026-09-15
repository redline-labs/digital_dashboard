#include "scope/add_signal_dialog.h"

#include "scope/signal_browser.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace scope
{

AddSignalDialog::AddSignalDialog(DataSource& source, const Panel& target, QWidget* parent) :
    QDialog(parent), target_(target)
{
    setObjectName("add_signal_dialog");
    setWindowTitle(tr("Add signal to %1").arg(target.title()));
    resize(480, 520);

    auto* layout = new QVBoxLayout(this);

    browser_ = new SignalBrowser(source, this);
    layout->addWidget(browser_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName("add_signal_buttons");
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Add"));
    // Nothing is selected yet, and a panel may refuse what is.
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Selecting records the candidate and gates OK on whether this particular
    // panel will take it, so "why can't I add this?" is answered before it is
    // asked rather than by an error afterwards.
    connect(browser_, &SignalBrowser::candidateActivated, this,
            [this, buttons](const BindingCandidate& candidate) {
                selected_ = candidate;
                const bool acceptable = target_.acceptsBinding(candidate);
                buttons->button(QDialogButtonBox::Ok)->setEnabled(acceptable);
                if (acceptable)
                {
                    // Double-clicking a usable field is the whole gesture; a
                    // second click on OK adds nothing.
                    accept();
                }
            });

    // Nothing to kick off: the browser is populated from advertisements the
    // moment it is constructed, and stays current on its own.
}

AddSignalDialog::~AddSignalDialog() = default;

}  // namespace scope

#include "scope/moc_add_signal_dialog.cpp"
