#ifndef BATTERYTELLTALEWIDGET_H
#define BATTERYTELLTALEWIDGET_H

#include <mercedes_190e_telltales/config.h>
#include <mercedes_190e_telltales/capnp_data_extractor.h>
#include "zenoh.hxx"

#include <QWidget>
#include <QPainter>
#include <QTimer>

#include <string_view>
#include <map>

// Declare metatype for Qt signal
Q_DECLARE_METATYPE(CapnpDataExtractor::VariableMap)

// Forward declaration.
class QSvgRenderer;

// ExprTk forward declaration
namespace exprtk {
    template <typename T> class expression;
    template <typename T> class symbol_table;
}


class Mercedes190EBatteryTelltale : public QWidget
{
    Q_OBJECT

public:
    using config_t = Mercedes190EBatteryTelltaleConfig_t;
    static constexpr std::string_view kWidgetName = "mercedes_190e_battery_telltale";

    explicit Mercedes190EBatteryTelltale(const Mercedes190EBatteryTelltaleConfig_t& cfg, QWidget *parent = nullptr);
    ~Mercedes190EBatteryTelltale();

    void setAsserted(bool asserted);
    
    // Set Zenoh session for data subscription
    void setZenohSession(std::shared_ptr<zenoh::Session> session);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onDataReceived(const CapnpDataExtractor::VariableMap& variables);

private:
    void updateColors();
    void initializeExpression();
    bool evaluateCondition();

    Mercedes190EBatteryTelltaleConfig_t _cfg;

    QSvgRenderer *mSvgRenderer;
    bool mAsserted;
    QColor mBackgroundColor;
    QColor mIconColor;
    
    // Colors
    static constexpr QColor kAssertedBackground = QColor(200, 50, 50);    // Medium red
    static constexpr QColor kNormalBackground = QColor(60, 60, 60);       // Dark gray
    static constexpr QColor kAssertedIcon = QColor(255, 255, 255);        // White when asserted
    static constexpr QColor kNormalIcon = QColor(120, 120, 120);          // Light gray when normal
    
        // Zenoh-related members
    std::shared_ptr<zenoh::Session> _zenoh_session;
    std::unique_ptr<zenoh::Subscriber<void>> _zenoh_subscriber;

    // Data extraction
    CapnpDataExtractor _data_extractor;

    // ExprTk-related members
    std::unique_ptr<exprtk::expression<double>> _expression;
    std::unique_ptr<exprtk::symbol_table<double>> _symbol_table;
    std::map<std::string, double> _variables;
    
    void createZenohSubscription();
};

#endif // BATTERYTELLTALEWIDGET_H 