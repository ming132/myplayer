#ifndef GL_WND_H
#define GL_WND_H

#include "HVideoWnd.h"
#include "HGLWidget.h"
//#include <iostream>
//#include <chrono>
class GLWnd : public HVideoWnd, HGLWidget {
public:
//    int o=0;
//    std::chrono::system_clock::time_point start = std::chrono::system_clock::now();
    GLWnd(QWidget* parent = nullptr);

    virtual void setGeometry(const QRect& rc) {
        HGLWidget::setGeometry(rc);
    }

    virtual void update() {
        HGLWidget::update();
    }

protected:
    virtual void paintGL();
    void drawTime();
    void drawFPS();
    void drawResolution();
};

#endif // GL_WND_H
