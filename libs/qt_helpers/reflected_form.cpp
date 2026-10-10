#include "qt_helpers/reflected_form.h"

namespace qt_helpers
{

QStringList ReflectedForm::problems() const
{
    QStringList out;
    for (const QLineEdit* line : findChildren<QLineEdit*>())
    {
        const QString problem = line->property(kProblemProperty).toString();
        // A removed list row is hidden at once and deleted later; it no longer
        // counts.
        if (problem.isEmpty() || !line->isVisibleTo(this))
        {
            continue;
        }
        QString path = line->objectName();
        path.remove(0, QStringLiteral("field:").size());
        out << QStringLiteral("%1: %2").arg(path, problem);
    }
    return out;
}

}  // namespace qt_helpers
