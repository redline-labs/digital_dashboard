#ifndef TELLTALEWIDGET_H
#define TELLTALEWIDGET_H

#include <mercedes_190e_telltales/config.h>

#include <QWidget>
#include <QPainter>
#include <QTimer>

#include <string_view>
#include <map>
#include <memory>

// Forward declarations
class QSvgRenderer;

namespace zenoh_subscriber {
    class ZenohSubscriber;
}


class Mercedes190ETelltale : public QWidget
{
    Q_OBJECT

public:
    using config_t = Mercedes190ETelltaleConfig_t;
    static constexpr std::string_view kWidgetName = "mercedes_190e_telltale";

    explicit Mercedes190ETelltale(const Mercedes190ETelltaleConfig_t& cfg, QWidget *parent = nullptr);
    const config_t& getConfig() const { return _cfg; }
    ~Mercedes190ETelltale();

    void setAsserted(bool asserted);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onConditionEvaluated(bool asserted);

private:
    void updateColors();

    Mercedes190ETelltaleConfig_t _cfg;

    QSvgRenderer *mSvgRenderer;
    bool mAsserted;
    QColor mBackgroundColor;
    QColor mIconColor;
    QString mSvgAlias;

    // Expression parser for condition evaluation
    std::unique_ptr<zenoh_subscriber::ZenohSubscriber> _expression_parser;
};

#endif // TELLTALEWIDGET_H


