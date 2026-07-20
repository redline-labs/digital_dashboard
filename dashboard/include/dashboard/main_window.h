#ifndef MAIN_WINDOW_H_
#define MAIN_WINDOW_H_

#include "app_config.h"

#include <QWidget>
#include <vector>
#include <memory>


class MainWindow : public QWidget
{
    Q_OBJECT

  public:
    MainWindow(const app_config_t& app_cfg);
    ~MainWindow();

    // Get the window name for identification
    const std::string& getWindowName() const;

  private:
    void createWidgetsFromConfig();

    app_config_t _app_cfg;
    std::vector<std::unique_ptr<QWidget>> _widgets;
};  // class MainWindow


#endif  // MAIN_WINDOW_H_

